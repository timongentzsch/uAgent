import { SETTINGS_AT } from "../shared/settings-topics.ts";
import { useState } from "preact/hooks";
import { OpenRouterSignIn } from "../shared/openrouter-signin.tsx";
import { Button } from "../shared/ui.tsx";
import { useResource } from "../shared/use-resource.ts";
import { useApi } from "../state/api.ts";
import { useHostContext } from "../state/host-context.ts";
import "./free-start.css";

export const freeStartId = "free-start";

// Whether the host has a provider to start a conversation with; asked only
// while `asked`.
export function useProvider(asked: boolean) {
  const { manage } = useApi();
  return useResource(() => (asked ? manage("provider") : undefined), [asked]);
}

// Shown while no provider is set up, and when a sign-in started anywhere
// comes back from OpenRouter.
export function FreeStart({
  online,
  provider,
}: {
  online: boolean;
  provider: ReturnType<typeof useProvider>;
}) {
  const { setModal } = useHostContext();
  // A sign-in that comes back from OpenRouter, or was just finished here,
  // shows its outcome whether or not the host had a provider.
  const [arriving] = useState(() =>
    new URLSearchParams(location.search).has("openrouter"),
  );
  const [signed, setSigned] = useState(false);
  const bare = arriving || signed;
  if (!bare && provider.value?.configured !== false) return null;
  return (
    <section
      id={freeStartId}
      class={bare ? undefined : "free-start"}
      aria-label={bare ? "OpenRouter sign-in" : "Start with a free model"}
    >
      {!bare && <strong>Start with a free model</strong>}
      {!bare && (
        <p class="muted">
          Sign in with OpenRouter. Free models cost nothing and have daily
          limits.
        </p>
      )}
      <OpenRouterSignIn
        key="sign-in"
        online={online}
        arrival
        done={() => {
          setSigned(true);
          provider.retry();
        }}
      >
        <Button
          onClick={() =>
            setModal({ type: "settings", address: SETTINGS_AT.providers })
          }
        >
          I have an API key
        </Button>
      </OpenRouterSignIn>
    </section>
  );
}
