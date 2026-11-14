// Copyright 2026 Timon Gentzsch
#ifndef UAGENT_INCLUDE_APP_OPENROUTER_SIGNIN_H_
#define UAGENT_INCLUDE_APP_OPENROUTER_SIGNIN_H_
// OpenRouter's sign-in (OAuth PKCE): the person approves on openrouter.ai and
// a one-time code is redeemed for a key, so no key is copied by hand.

#include <string>
#include <string_view>

namespace uagent {

// The S256 challenge of `verifier`: base64url of its SHA-256.
std::string PkceChallenge(std::string_view verifier);

struct SignIn {
  // Where the person approves.
  std::string url;
  // Stays here; the code is worth nothing without it.
  std::string verifier;
  // Comes back with a redirect, to tell ours from another's.
  std::string state;
};

// With a `callback` OpenRouter sends the browser there with the code; with
// none it shows the code to paste. `base_url` is the API's.
SignIn BeginSignIn(const std::string& base_url, const std::string& callback);

// The key `code` is good for, or empty with `error`.
std::string RedeemSignIn(const std::string& base_url, const std::string& code,
                         const std::string& verifier, std::string& error);

// A listener on a loopback port for the redirect; -1 when none could be
// opened. `callback` is the address to give BeginSignIn.
int ListenForSignIn(std::string& callback);

// Waits on `listener` for the redirect carrying `state` and closes it: the
// code, or empty when none came in `seconds`.
std::string AcceptSignIn(int listener, const std::string& state, int seconds);

}  // namespace uagent
#endif  // UAGENT_INCLUDE_APP_OPENROUTER_SIGNIN_H_
