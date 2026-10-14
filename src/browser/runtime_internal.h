// Copyright 2026 Timon Gentzsch

#ifndef UAGENT_SRC_BROWSER_RUNTIME_INTERNAL_H_
#define UAGENT_SRC_BROWSER_RUNTIME_INTERNAL_H_
// Module-private split of the browser runtime. The Runtime class itself is in
// include/browser/runtime.h; its methods are defined across runtime.cc
// (lifecycle, dispatch, page selection), runtime_control.cc (profiles and
// hand-over), runtime_act.cc (page actions) and runtime_page.cc (reading and
// observing). What more than one of them needs is declared here.

#include <sys/types.h>

#include <string>

#include "include/core/json.h"

namespace uagent::browser {

constexpr const char* kHumanControls =
    "the user controls the browser. Call request_human to wait for their "
    "hand-back, or continue without the browser";

// Draws every password field as dots in every screenshot, including one a
// site's reveal toggle has switched to text: an input that ever was a
// password stays tagged. Autofill still works, since only drawing changes.
// Set through the CSSOM, which a page's CSP does not govern, and set again
// whenever the page rewrites the style or drops the tag. Each write is
// guarded, since the observer sees its own writes too. Installed on each
// new document and run once more before a capture, for a page that loaded
// before the attach. Not covered: shadow roots and cross-site iframes, which
// run out of process and would need Target.setAutoAttach to reach.
constexpr const char* kSecretScript =
    "(()=>{if(window.__uagentSecret)return;window.__uagentSecret=1;"
    "const T='data-uagent-secret',P='-webkit-text-security';"
    "const tag=(n,was)=>{if(n.tagName!=='INPUT')return;"
    "if(!n.hasAttribute(T)&&(was||n.type==='password'||"
    "/password/i.test(n.getAttribute('autocomplete')||'')))"
    "n.setAttribute(T,'');"
    "if(n.hasAttribute(T)&&n.style.getPropertyValue(P)!=='disc')"
    "n.style.setProperty(P,'disc','important')};"
    "const scan=n=>{if(n.nodeType!==1)return;tag(n);"
    "n.querySelectorAll('input').forEach(i=>tag(i))};"
    "new MutationObserver(m=>{for(const r of m){if(r.type==='attributes')"
    "tag(r.target,r.attributeName==='type'?r.oldValue==='password':"
    "r.attributeName===T&&r.oldValue!==null);"
    "else r.addedNodes.forEach(scan)}}).observe(document,{subtree:true,"
    "childList:true,attributes:true,attributeOldValue:true,"
    "attributeFilter:['type','autocomplete','style',T]});"
    "if(document.documentElement)scan(document.documentElement)})()";

constexpr const char* kLeasedElsewhere =
    "another conversation is using the browser; it is free again when that "
    "conversation's turn ends";

bool Alive(pid_t& pid);
std::string CdError(const json& value);
std::string ProfileName(std::string name);

}  // namespace uagent::browser

#endif  // UAGENT_SRC_BROWSER_RUNTIME_INTERNAL_H_
