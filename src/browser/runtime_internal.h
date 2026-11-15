// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_SRC_BROWSER_RUNTIME_INTERNAL_H_
#define UAGENT_SRC_BROWSER_RUNTIME_INTERNAL_H_
// What more than one of the runtime*.cc files needs; the Runtime class itself
// is in include/browser/runtime.h.

#include <sys/types.h>

#include <string>
#include <vector>

#include "include/core/json.h"

namespace uagent::browser {

constexpr const char* kHumanControls =
    "the user controls the browser. Call request_human to wait for their "
    "hand-back, or continue without the browser";

// Every script runs in a world of its own, as an extension's content script
// does: the page sees none of its globals and cannot replace them.
constexpr const char* kWorld = "uagent";

// Draws every password field as dots in screenshots, also one a reveal toggle
// switched to text. Set through the CSSOM, which a page's CSP does not govern.
// Not covered: closed shadow roots and frames of other sites.
constexpr const char* kSecretScript =
    "(()=>{if(window.__uagentSecret)return window.__uagentSecret();"
    "const T='data-uagent-secret',P='-webkit-text-security';"
    "const tag=(n,was)=>{if(n.tagName!=='INPUT')return;"
    "if(!n.hasAttribute(T)&&(was||n.type==='password'||"
    "/password/i.test(n.getAttribute('autocomplete')||'')))"
    "n.setAttribute(T,'');"
    "if(n.hasAttribute(T)&&n.style.getPropertyValue(P)!=='disc')"
    "n.style.setProperty(P,'disc','important')};"
    "const all={subtree:true,childList:true,attributes:true,"
    "attributeOldValue:true,attributeFilter:['type','autocomplete','style',T]};"
    "const o=new MutationObserver(m=>{for(const r of m){"
    "if(r.type==='attributes')"
    "tag(r.target,r.attributeName==='type'?r.oldValue==='password':"
    "r.attributeName===T&&r.oldValue!==null);"
    "else r.addedNodes.forEach(scan)}});"
    "const roots=new WeakSet();"
    "const watch=r=>{if(!roots.has(r)){roots.add(r);o.observe(r,all)}"
    "scan(r)};"
    "const into=n=>{tag(n);if(n.shadowRoot)watch(n.shadowRoot);"
    "if(/^I?FRAME$/.test(n.tagName))"
    "try{n.contentWindow.__uagentSecret()}catch(_){}};"
    "const scan=n=>{if(n.nodeType===1)into(n);else if(n.nodeType!==11)return;"
    "n.querySelectorAll('*').forEach(into)};"
    "o.observe(document,all);"
    "Object.defineProperty(window,'__uagentSecret',{value:()=>{"
    "if(document.documentElement)scan(document.documentElement)}});"
    "window.__uagentSecret()})()";

constexpr const char* kLeasedElsewhere =
    "another conversation is using the browser; it is free again when that "
    "conversation's turn ends";

bool Alive(pid_t& pid);
std::string CdError(const json& value);
// The RemoteObject a Runtime.evaluate or Runtime.callFunctionOn reply holds,
// and its value when that is an object.
inline const json* RemoteObject(const json& reply) {
  const json* result = JsonObject(reply, "result");
  return result ? JsonObject(*result, "result") : nullptr;
}
inline const json* RemoteValue(const json& reply) {
  const json* remote = RemoteObject(reply);
  return remote ? JsonObject(*remote, "value") : nullptr;
}
std::string ProfileName(std::string name);
// Appends a line for a console event: a logged message or an uncaught error.
void Logged(const std::string& event, const json& params,
            std::vector<std::string>& lines);

}  // namespace uagent::browser

#endif  // UAGENT_SRC_BROWSER_RUNTIME_INTERNAL_H_
