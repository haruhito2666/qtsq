chrome.runtime.onInstalled.addListener(() => {
    const viewerUrl = chrome.runtime.getURL("viewer.html?url=");
    
    chrome.declarativeNetRequest.updateDynamicRules({
        removeRuleIds: [1, 2],
        addRules: [
            {
                id: 1,
                priority: 1,
                action: {
                    type: "redirect",
                    redirect: {
                        regexSubstitution: viewerUrl + "\\0"
                    }
                },
                condition: {
                    regexFilter: "^(https?://.*\\.qtsq)(\\?[^#]*)?(#.*)?$",
                    resourceTypes: ["main_frame"]
                }
            },
            {
                id: 2,
                priority: 1,
                action: {
                    type: "redirect",
                    redirect: {
                        regexSubstitution: viewerUrl + "\\0"
                    }
                },
                condition: {
                    regexFilter: "^(file://.*\\.qtsq)(\\?[^#]*)?(#.*)?$",
                    resourceTypes: ["main_frame"]
                }
            }
        ]
    });
});
