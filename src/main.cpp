#include <Geode/Geode.hpp>
#include <Geode/modify/MenuLayer.hpp>
#include <Geode/ui/Popup.hpp>
#include <Geode/ui/ScrollLayer.hpp>
#include <Geode/ui/TextInput.hpp>
#include <Geode/ui/GeodeUI.hpp> 
#include <windows.h>
#include <thread>
#include <vector>
#include <string>
#include <algorithm>
#include <sstream>
#include <fstream>

using namespace geode::prelude;
using namespace cocos2d;
using namespace cocos2d::extension;

#define GD_CURLOPT_WRITEDATA 10001
#define GD_CURLOPT_URL 10002
#define GD_CURLOPT_POSTFIELDS 10015
#define GD_CURLOPT_HTTPHEADER 10023
#define GD_CURLOPT_WRITEFUNCTION 20011
#define GD_CURLOPT_SSL_VERIFYPEER 64
#define GD_CURLINFO_RESPONSE_CODE 2097154

typedef void* (*curl_easy_init_t)();
typedef int (*curl_easy_setopt_t)(void*, int, ...);
typedef int (*curl_easy_perform_t)(void*);
typedef void (*curl_easy_cleanup_t)(void*);
typedef void* (*curl_slist_append_t)(void*, const char*);
typedef void (*curl_slist_free_all_t)(void*);
typedef int (*curl_easy_getinfo_t)(void*, int, ...);
typedef const char* (*curl_easy_strerror_t)(int);

static size_t curlWriteCallback(void* contents, size_t size, size_t nmemb, void* userp) {
    ((std::string*)userp)->append((char*)contents, size * nmemb);
    return size * nmemb;
}

struct ChatMessage {
    std::string content;
    bool isUser;
};

namespace ChatMemory {
    std::vector<ChatMessage> history;
    std::string getSaveFilePath() {
        return (geode::Mod::get()->getSaveDir() / "gd_gpt_chat_history.txt").string();
    }
}

void performSaveChatHistory() {
    if (!geode::Mod::get()->getSettingValue<bool>("save-chat")) return;
    std::string serializedData = "";
    for (const auto& message : ChatMemory::history) {
        serializedData += (message.isUser ? "1" : "0");
        serializedData += message.content;
        serializedData += "\x1E"; 
    }
    std::ofstream outFile(ChatMemory::getSaveFilePath(), std::ios::binary | std::ios::trunc);
    if (outFile.is_open()) {
        outFile.write(serializedData.c_str(), serializedData.size());
        outFile.close();
    }
}

void performLoadChatHistory() {
    ChatMemory::history.clear();
    std::ifstream inFile(ChatMemory::getSaveFilePath(), std::ios::binary);
    if (!inFile.is_open()) return;
    std::stringstream buffer;
    buffer << inFile.rdbuf();
    std::string rawHistoryString = buffer.str();
    inFile.close();
    if (rawHistoryString.empty()) return;
    size_t currentPos = 0;
    while (currentPos < rawHistoryString.length()) {
        size_t endOfRecord = rawHistoryString.find('\x1E', currentPos);
        if (endOfRecord == std::string::npos) break;
        std::string singleRecord = rawHistoryString.substr(currentPos, endOfRecord - currentPos);
        if (singleRecord.length() > 1) {
            bool messageIsFromUser = (singleRecord[0] == '1');
            std::string messageText = singleRecord.substr(1);
            ChatMemory::history.push_back({messageText, messageIsFromUser});
        }
        currentPos = endOfRecord + 1;
    }
}

class ClearChatSetting;
class ClearChatNodeV3 : public geode::SettingNodeV3 {
protected:
    bool init(std::shared_ptr<ClearChatSetting> setting, float width) {
        auto baseSetting = std::static_pointer_cast<geode::SettingV3>(setting);
        if (!geode::SettingNodeV3::init(baseSetting, width)) return false;
        this->setContentSize({ width, 35.f });
        auto menu = CCMenu::create();
        menu->setPosition({ width - 40.f, 17.5f });
        this->addChild(menu);
        auto btnSprite = ButtonSprite::create("Clear", "bigFont.fnt", "GJ_button_05.png", 0.4f);
        auto clearBtn = CCMenuItemSpriteExtra::create(
            btnSprite, this, menu_selector(ClearChatNodeV3::onExecuteClear)
        );
        clearBtn->setScale(0.6f); 
        menu->addChild(clearBtn);
        return true;
    }
public:
    void onExecuteClear(CCObject*) {
        ChatMemory::history.clear();
        std::ofstream outFile(ChatMemory::getSaveFilePath(), std::ios::trunc);
        if (outFile.is_open()) outFile.close();
        FLAlertLayer::create("Success", "All chat history has been deleted.", "OK")->show();
    }
    void onCommit() override {}
    void onResetToDefault() override {}
    bool hasUncommittedChanges() const override { return false; }
    bool hasNonDefaultValue() const override { return false; }
    static ClearChatNodeV3* create(std::shared_ptr<ClearChatSetting> setting, float width) {
        auto ret = new ClearChatNodeV3();
        if (ret && ret->init(setting, width)) {
            ret->autorelease(); return ret;
        }
        CC_SAFE_DELETE(ret); return nullptr;
    }
};

class ClearChatSetting : public geode::SettingV3 {
public:
    static Result<std::shared_ptr<SettingV3>> parse(std::string const& key, std::string const& modID, matjson::Value const& json) {
        auto res = std::make_shared<ClearChatSetting>();
        res->init(key, modID);
        return geode::Ok(std::static_pointer_cast<geode::SettingV3>(res));
    }
    bool load(matjson::Value const& json) override { return true; }
    bool save(matjson::Value& json) const override { return true; }
    bool isDefaultValue() const override { return true; }
    void reset() override {}
    geode::SettingNodeV3* createNode(float width) override {
        auto sharedSelf = std::static_pointer_cast<ClearChatSetting>(shared_from_this());
        return ClearChatNodeV3::create(sharedSelf, width);
    }
};

$execute {
    (void)geode::Mod::get()->registerCustomSettingType("clear-chat", &ClearChatSetting::parse);
}

class GptPopup : public geode::Popup {
protected:
    geode::TextInput* m_inputField = nullptr;
    geode::ScrollLayer* m_scrollArea = nullptr;
    CCLabelBMFont* m_currentTypewriterLabel = nullptr;
    std::string m_fullTargetText = "";
    int m_animationCharIdx = 0;

    std::string stripMarkdown(std::string text) {
        std::vector<std::string> targets = {"**", "###", "##", "`"};
        for (const auto& target : targets) {
            size_t pos;
            while ((pos = text.find(target)) != std::string::npos) {
                text.erase(pos, target.length());
            }
        }
        return text;
    }

    std::string processWordWrap(const std::string& text, size_t lineLimit) {
        std::string result = "";
        size_t currentLineLength = 0;
        std::stringstream ss(text);
        std::string word;
        while (ss >> word) {
            if (currentLineLength + word.length() > lineLimit) {
                result += "\n";
                currentLineLength = 0;
            }
            result += word + " ";
            currentLineLength += word.length() + 1;
        }
        return result;
    }

    std::string sanitizeForJson(const std::string& input) {
        std::string output = "";
        for (auto c : input) {
            if (c == '"') output += "\\\"";
            else if (c == '\\') output += "\\\\";
            else if (c == '\n') output += "\\n";
            else output += c;
        }
        return output;
    }

    bool init() override {
        if (!geode::Popup::init(400.f, 280.f, "GJ_square01.png")) return false;
        this->setOpacity(150);
        this->setTitle(""); 
        auto logo = CCSprite::create("Icon.png"_spr);
        if (!logo) logo = CCSprite::createWithSpriteFrameName("GJ_infoIcon_001.png");
        logo->setScale(0.85f);
        logo->setPosition({ 200.f, 255.f });
        m_mainLayer->addChild(logo, 10);
        m_scrollArea = geode::ScrollLayer::create({ 360.f, 160.f });
        m_scrollArea->setPosition({ 20.f, 70.f });
        m_mainLayer->addChild(m_scrollArea, 5);
        m_inputField = geode::TextInput::create(290.f, "Ask Gemini or type /list...", "chatFont.fnt");
        m_inputField->setPosition({ 180.f, 35.f });
        if (auto bg = m_inputField->getChildByID("bg")) bg->setVisible(false);
        if (auto inputNode = m_inputField->getInputNode()) {
            inputNode->setMaxLabelLength(500);
            inputNode->setAllowedChars("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789!@#$%^&*()-=_+~`[]{}\\|;:'\",.<>/? ");
        }
        m_mainLayer->addChild(m_inputField, 10);
        auto sendSprite = CCSprite::create("Send.png"_spr);
        if (!sendSprite) sendSprite = CCSprite::createWithSpriteFrameName("GJ_playBtn2_001.png");
        sendSprite->setScale(35.f / sendSprite->getContentSize().width);
        auto sendBtn = CCMenuItemSpriteExtra::create(
            sendSprite, this, menu_selector(GptPopup::onSendMessageAction)
        );
        sendBtn->setPosition({ 360.f, 35.f });
        m_buttonMenu->addChild(sendBtn);
        auto settingsSprite = CCSprite::createWithSpriteFrameName("GJ_optionsBtn_001.png");
        settingsSprite->setScale(0.7f);
        auto settingsBtn = CCMenuItemSpriteExtra::create(
            settingsSprite, this, menu_selector(GptPopup::onOpenSettings)
        );
        settingsBtn->setPosition({ 370.f, 255.f });
        m_buttonMenu->addChild(settingsBtn);
        this->addDecorationCorners();
        performLoadChatHistory();
        for (const auto& msg : ChatMemory::history) {
            this->createChatMessageBubble(msg.content, msg.isUser);
        }
        return true;
    }

    void addDecorationCorners() {
        auto yellow = ccColor3B{ 255, 205, 90 };
        auto tl = CCSprite::createWithSpriteFrameName("dailyLevelCorner_001.png"); tl->setRotation(90); tl->setPosition({ 24, 256 }); tl->setColor(yellow); m_mainLayer->addChild(tl, 20);
        auto tr = CCSprite::createWithSpriteFrameName("dailyLevelCorner_001.png"); tr->setRotation(180); tr->setPosition({ 376, 256 }); tr->setColor(yellow); m_mainLayer->addChild(tr, 20);
        auto bl = CCSprite::createWithSpriteFrameName("dailyLevelCorner_001.png"); bl->setPosition({ 24, 24 }); bl->setColor(yellow); m_mainLayer->addChild(bl, 20);
        auto br = CCSprite::createWithSpriteFrameName("dailyLevelCorner_001.png"); br->setRotation(-90); br->setPosition({ 376, 24 }); br->setColor(yellow); m_mainLayer->addChild(br, 20);
    }

    void onOpenSettings(CCObject*) {
        geode::openSettingsPopup(geode::Mod::get());
    }

    CCLabelBMFont* createChatMessageBubble(const std::string& text, bool isUser) {
        auto contentLayer = m_scrollArea->m_contentLayer;
        float fontScale = 0.55f;
        auto label = CCLabelBMFont::create(text.c_str(), "chatFont.fnt");
        label->setScale(fontScale);
        label->setAlignment(kCCTextAlignmentCenter);
        float bWidth = std::max(55.f, label->getContentSize().width * fontScale + 32.f);
        float bHeight = label->getContentSize().height * fontScale + 28.f;
        auto bubble = CCScale9Sprite::create(isUser ? "GJ_square02.png" : "GJ_square01.png");
        bubble->setContentSize({ bWidth, bHeight });
        bubble->setTag(isUser ? 1 : 0);
        label->setPosition({ bWidth / 2.f, bHeight / 2.f + 1.f });
        bubble->addChild(label);
        contentLayer->addChild(bubble);
        this->updateChatLayout();
        return label;
    }

    void updateChatLayout() {
        auto layer = m_scrollArea->m_contentLayer;
        float padding = 8.f;
        float currentTotalH = padding;
        for (int i = 0; i < layer->getChildrenCount(); ++i) {
            auto child = static_cast<CCNode*>(layer->getChildren()->objectAtIndex(i));
            currentTotalH += child->getContentSize().height + padding;
        }
        layer->setContentSize({ 360.f, std::max(currentTotalH, 160.f) });
        float yPointer = layer->getContentSize().height - padding;
        for (int i = 0; i < layer->getChildrenCount(); ++i) {
            auto node = static_cast<CCNode*>(layer->getChildren()->objectAtIndex(i));
            float xPos = node->getTag() == 1 ? 355.f - node->getContentSize().width / 2.f : 5.f + node->getContentSize().width / 2.f;
            node->setPosition({ xPos, yPointer - node->getContentSize().height / 2.f });
            yPointer -= node->getContentSize().height + padding;
        }
        layer->setPositionY(0);
    }

    void onSendMessageAction(CCObject*) {
        if (m_scrollArea->m_contentLayer->getChildByTag(9999)) return;
        std::string rawInput = m_inputField->getString();
        if (rawInput.empty()) return;
        bool isListCommand = (rawInput == "/list");
        std::string wrappedText = processWordWrap(rawInput, 45);
        ChatMemory::history.push_back({ wrappedText, true });
        performSaveChatHistory();
        this->createChatMessageBubble(wrappedText, true);
        m_inputField->setString("");
        auto thinkingLbl = this->createChatMessageBubble(isListCommand ? "Fetching..." : "Thinking...", false);
        thinkingLbl->getParent()->setTag(9999);
        std::string apiKey = geode::Mod::get()->getSettingValue<std::string>("api-key");
        apiKey.erase(std::remove_if(apiKey.begin(), apiKey.end(), [](unsigned char c){ return std::isspace(c); }), apiKey.end());
        if (apiKey.empty()) {
            this->onHttpResponseArrived("Error: No API Key.", 401, false, false);
            return;
        }
        std::string postPayload = "";
        std::string url = "";
        if (isListCommand) {
            url = "https://generativelanguage.googleapis.com/v1beta/models?key=" + apiKey;
        } else {
            postPayload = "{ \"contents\": [";
            int startIndex = std::max(0, static_cast<int>(ChatMemory::history.size()) - 6);
            bool isFirstItem = true;
            for (int i = startIndex; i < ChatMemory::history.size(); ++i) {
                if (!isFirstItem) postPayload += ",";
                isFirstItem = false;
                std::string roleName = ChatMemory::history[i].isUser ? "user" : "model";
                std::string textContent = sanitizeForJson(ChatMemory::history[i].content);
                if (i == startIndex && roleName == "user") {
                    textContent = "You are an AI inside Geometry Dash. Keep answers brief. Do NOT use markdown like ** or ##.\\nIf the user asks you to create a level, output the raw Geometry Dash level string enclosed in <LEVEL> and </LEVEL>. Example: <LEVEL>1,1,2,15,3,15;</LEVEL>\\n\\n" + textContent;
                }
                postPayload += "{\"role\": \"" + roleName + "\", \"parts\": [{\"text\": \"" + textContent + "\"}]}";
            }
            postPayload += "]}";
            url = "https://generativelanguage.googleapis.com/v1/models/gemini-2.0-flash-lite:generateContent";
        }
        this->retain();
        std::thread([this, url, postPayload, apiKey, isListCommand]() {
            HMODULE hCurl = GetModuleHandleA("libcurl.dll");
            if (!hCurl) return;
            auto p_curl_easy_init = (curl_easy_init_t)GetProcAddress(hCurl, "curl_easy_init");
            auto p_curl_easy_setopt = (curl_easy_setopt_t)GetProcAddress(hCurl, "curl_easy_setopt");
            auto p_curl_easy_perform = (curl_easy_perform_t)GetProcAddress(hCurl, "curl_easy_perform");
            auto p_curl_easy_cleanup = (curl_easy_cleanup_t)GetProcAddress(hCurl, "curl_easy_cleanup");
            auto p_curl_slist_append = (curl_slist_append_t)GetProcAddress(hCurl, "curl_slist_append");
            auto p_curl_slist_free_all = (curl_slist_free_all_t)GetProcAddress(hCurl, "curl_slist_free_all");
            auto p_curl_easy_getinfo = (curl_easy_getinfo_t)GetProcAddress(hCurl, "curl_easy_getinfo");
            auto p_curl_easy_strerror = (curl_easy_strerror_t)GetProcAddress(hCurl, "curl_easy_strerror");
            if (!p_curl_easy_init) return;
            void* curl = p_curl_easy_init();
            std::string readBuffer = "";
            long http_code = 0;
            int res = -1;
            if(curl) {
                p_curl_easy_setopt(curl, GD_CURLOPT_URL, url.c_str());
                p_curl_easy_setopt(curl, GD_CURLOPT_SSL_VERIFYPEER, 0L);
                void* headers = nullptr;
                if (!isListCommand) {
                    p_curl_easy_setopt(curl, GD_CURLOPT_POSTFIELDS, postPayload.c_str());
                    headers = p_curl_slist_append(headers, "Content-Type: application/json");
                    std::string headerApiKey = "x-goog-api-key: " + apiKey;
                    headers = p_curl_slist_append(headers, headerApiKey.c_str());
                    p_curl_easy_setopt(curl, GD_CURLOPT_HTTPHEADER, headers);
                }
                p_curl_easy_setopt(curl, GD_CURLOPT_WRITEFUNCTION, curlWriteCallback);
                p_curl_easy_setopt(curl, GD_CURLOPT_WRITEDATA, &readBuffer);
                res = p_curl_easy_perform(curl);
                p_curl_easy_getinfo(curl, GD_CURLINFO_RESPONSE_CODE, &http_code);
                if (headers) p_curl_slist_free_all(headers);
                p_curl_easy_cleanup(curl);
            }
            geode::Loader::get()->queueInMainThread([this, res, readBuffer, http_code, p_curl_easy_strerror, isListCommand]() {
                if (res != 0 && p_curl_easy_strerror) {
                    this->onHttpResponseArrived("CURL Error: " + std::string(p_curl_easy_strerror(res)), http_code, false, isListCommand);
                } else {
                    bool isSuccess = (http_code >= 200 && http_code < 300);
                    this->onHttpResponseArrived(readBuffer, http_code, isSuccess, isListCommand);
                }
                this->release();
            });
        }).detach();
    }

    void onHttpResponseArrived(std::string rawResponse, int statusCode, bool isOk, bool isListCommand) {
        std::string finalMessage = "No response from server.";
        bool shouldSave = isOk;
        if (isOk) {
            auto parseResult = matjson::parse(rawResponse);
            if (parseResult.isOk()) {
                auto json = parseResult.unwrap();
                if (isListCommand) {
                    finalMessage = "Models Available:\n";
                    if (json.contains("models") && json["models"].isArray()) {
                        auto models = json["models"].asArray().unwrap();
                        for (auto& m : models) {
                            if (m.contains("name")) {
                                std::string name = m["name"].asString().unwrapOr("");
                                if (name.find("gemini") != std::string::npos) {
                                    finalMessage += "- " + name + "\n";
                                }
                            }
                        }
                    } else {
                        finalMessage = "Error: 'models' list not found.";
                    }
                    shouldSave = false; 
                } 
                else {
                    if (json.contains("candidates") && json["candidates"].isArray() && json["candidates"].asArray().unwrap().size() > 0) {
                        auto candidate = json["candidates"][0];
                        if (candidate.contains("content") && candidate["content"].contains("parts") && candidate["content"]["parts"].isArray()) {
                            finalMessage = candidate["content"]["parts"][0]["text"].asString().unwrapOr("Error reading Gemini text.");
                        } else {
                            finalMessage = "JSON Parse Error: Missing content parts.";
                            shouldSave = false;
                        }
                    } else {
                        finalMessage = "JSON Parse Error: Missing candidates.";
                        shouldSave = false;
                    }
                }
            } else {
                finalMessage = "Failed to parse API JSON.";
                shouldSave = false;
            }
        } else {
            finalMessage = "API Error " + std::to_string(statusCode);
            shouldSave = false;
        }
        this->onFinalizeResponse(finalMessage, shouldSave);
    }

    void onFinalizeResponse(std::string text, bool shouldSave) {
        if (!this->getParent()) {
            if (shouldSave) {
                ChatMemory::history.push_back({ processWordWrap(text, 45), false });
                performSaveChatHistory();
            }
            return;
        }
        if (auto thinkingBubble = m_scrollArea->m_contentLayer->getChildByTag(9999)) {
            thinkingBubble->removeFromParent();
        }
        size_t levelStart = text.find("<LEVEL>");
        size_t levelEnd = text.find("</LEVEL>");
        if (levelStart != std::string::npos && levelEnd != std::string::npos && levelEnd > levelStart) {
            std::string levelStr = text.substr(levelStart + 7, levelEnd - levelStart - 7);
            auto glm = GameLevelManager::sharedState();
            auto newLevel = glm->createNewLevel();
            newLevel->m_levelName = "AI Generated";
            newLevel->m_levelString = levelStr;
            text.erase(levelStart, levelEnd - levelStart + 8);
            text += "\n[Level has been generated in your Create tab!]";
            FLAlertLayer::create("AI Level Creator", "The AI successfully created a new level!", "Awesome!")->show();
        }
        text = stripMarkdown(text);
        m_fullTargetText = processWordWrap(text, 45);
        if (shouldSave) {
            ChatMemory::history.push_back({ m_fullTargetText, false });
            performSaveChatHistory();
        }
        m_currentTypewriterLabel = this->createChatMessageBubble("", false);
        m_animationCharIdx = 0;
        this->schedule(schedule_selector(GptPopup::onTypewriterTick), 0.02f);
    }

    void onTypewriterTick(float dt) {
        if (m_animationCharIdx < m_fullTargetText.length()) {
            unsigned char c = m_fullTargetText[m_animationCharIdx];
            int charLen = 1;
            if ((c & 0xE0) == 0xC0) charLen = 2;
            else if ((c & 0xF0) == 0xE0) charLen = 3;
            else if ((c & 0xF8) == 0xF0) charLen = 4;
            m_animationCharIdx += charLen;
            if (m_animationCharIdx > m_fullTargetText.length()) m_animationCharIdx = m_fullTargetText.length();
            m_currentTypewriterLabel->setString(m_fullTargetText.substr(0, m_animationCharIdx).c_str());
            float fontScale = 0.55f;
            float newWidth = std::max(55.f, m_currentTypewriterLabel->getContentSize().width * fontScale + 32.f);
            float newHeight = m_currentTypewriterLabel->getContentSize().height * fontScale + 28.f;
            auto bubbleSprite = static_cast<CCScale9Sprite*>(m_currentTypewriterLabel->getParent());
            if (bubbleSprite) {
                bubbleSprite->setContentSize({ newWidth, newHeight });
                m_currentTypewriterLabel->setPosition({ newWidth / 2.f, newHeight / 2.f + 1.f });
            }
            this->updateChatLayout();
        } else {
            this->unschedule(schedule_selector(GptPopup::onTypewriterTick));
        }
    }

public:
    static GptPopup* create() {
        auto ret = new GptPopup();
        if (ret && ret->init()) {
            ret->autorelease(); return ret;
        }
        CC_SAFE_DELETE(ret); return nullptr;
    }
};

class FloatingIconButton : public CCSprite, public CCTargetedTouchDelegate {
protected:
    bool m_dragActive = false;
    CCPoint m_touchOffset;
    CCPoint m_initialPos;
    CCObject* m_targetObj;
    SEL_MenuHandler m_callbackMethod;
public:
    static FloatingIconButton* create(const char* frameName, CCObject* target, SEL_MenuHandler callback) {
        auto ret = new FloatingIconButton();
        if (ret && ret->initWithSpriteFrameName(frameName)) {
            ret->autorelease(); 
            ret->m_targetObj = target; 
            ret->m_callbackMethod = callback; 
            ret->setScale(0.55f); 
            return ret;
        }
        CC_SAFE_DELETE(ret); return nullptr;
    }
    void onEnter() override {
        CCSprite::onEnter(); 
        CCDirector::sharedDirector()->getTouchDispatcher()->addTargetedDelegate(this, -130, true); 
    }
    void onExit() override {
        CCDirector::sharedDirector()->getTouchDispatcher()->removeDelegate(this); 
        CCSprite::onExit(); 
    }
    bool ccTouchBegan(CCTouch* touch, CCEvent* event) override {
        auto touchLocation = this->getParent()->convertToNodeSpace(touch->getLocation());
        if (this->boundingBox().containsPoint(touchLocation)) {
            m_dragActive = true; 
            m_touchOffset = touchLocation; 
            m_initialPos = this->getPosition(); 
            this->setOpacity(170); 
            return true;
        }
        return false;
    }
    void ccTouchMoved(CCTouch* touch, CCEvent* event) override {
        if (!m_dragActive) return; 
        auto touchLocation = this->getParent()->convertToNodeSpace(touch->getLocation());
        auto delta = ccpSub(touchLocation, m_touchOffset);
        auto newPosition = ccpAdd(m_initialPos, delta);
        auto winSize = CCDirector::sharedDirector()->getWinSize();
        newPosition.x = std::max(22.f, std::min(newPosition.x, winSize.width - 22.f));
        newPosition.y = std::max(22.f, std::min(newPosition.y, winSize.height - 22.f));
        this->setPosition(newPosition);
    }
    void ccTouchEnded(CCTouch* touch, CCEvent* event) override {
        if (!m_dragActive) return; 
        m_dragActive = false; 
        this->setOpacity(255);
        auto endPoint = this->getParent()->convertToNodeSpace(touch->getLocation());
        if (endPoint.getDistance(m_touchOffset) < 12.f) {
            if (m_targetObj && m_callbackMethod) (m_targetObj->*m_callbackMethod)(this);
        } else {
            geode::Mod::get()->setSavedValue("icon-x", this->getPositionX()); 
            geode::Mod::get()->setSavedValue("icon-y", this->getPositionY());
        }
    }
};

class $modify(MainGptMenuLayer, MenuLayer) {
    bool init() {
        if (!MenuLayer::init()) return false;
        auto winSize = CCDirector::sharedDirector()->getWinSize();
        auto chatButton = FloatingIconButton::create("GJ_chatBtn_001.png", this, menu_selector(MainGptMenuLayer::onOpenChatWindow));
        float xPos = geode::Mod::get()->getSavedValue<float>("icon-x", winSize.width - 45.f);
        float yPos = geode::Mod::get()->getSavedValue<float>("icon-y", winSize.height / 2.f);
        chatButton->setPosition({ xPos, yPos });
        this->addChild(chatButton, 110); 
        return true;
    }
    void onOpenChatWindow(CCObject*) {
        auto currentScene = CCDirector::sharedDirector()->getRunningScene();
        if (currentScene && !currentScene->getChildByTag(99999)) {
            auto newPopup = GptPopup::create();
            newPopup->setTag(99999);
            newPopup->show();
        }
    }
};