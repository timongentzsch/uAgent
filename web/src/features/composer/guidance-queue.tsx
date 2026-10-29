import { useState } from "preact/hooks";
import type { Outgoing } from "../../shared/types.ts";
import { outgoingStatus } from "../../shared/message-view.ts";
import { Button } from "../../shared/ui.tsx";
import { plural } from "../../shared/quantities.ts";

// Pending input stays with the composer. Only the host's delivered message
// joins the transcript; command receipts alone never remove a queued item.
export default function GuidanceQueue({
  items,
  online,
  held,
  recall,
  insert,
}: {
  items: Outgoing[];
  online: boolean;
  held: boolean;
  recall?: (item: Outgoing) => Promise<void>;
  insert?: (item: Outgoing) => Promise<void>;
}) {
  const [working, setWorking] = useState<Set<string>>(new Set());
  if (!items.length) return null;
  const run = async (
    item: Outgoing,
    action: (item: Outgoing) => Promise<void>,
  ) => {
    setWorking((prior) => new Set(prior).add(item.id));
    try {
      await action(item);
    } finally {
      setWorking((prior) => new Set([...prior].filter((id) => id !== item.id)));
    }
  };
  return (
    <section class="guidance-queue" aria-label="Queued messages">
      <div class="queue-heading" role="status">
        {plural(items.length, "message")} queued
      </div>
      <div class="queue-items divided">
        {items.map((item) => (
          <article
            class="queue-item"
            key={item.id}
            aria-busy={working.has(item.id)}
          >
            <p class="queue-text">{item.body?.text}</p>
            {!!item.files?.length && (
              <p class="queue-files muted">
                {item.files.map((file) => file.name).join(", ")}
              </p>
            )}
            <div class="queue-controls">
              <small class="muted">
                {item.status === outgoingStatus.next
                  ? "After this turn"
                  : "Next checkpoint"}
              </small>
              <div>
                {recall && (
                  <Button
                    variant="quiet"
                    size="compact"
                    disabled={!online || working.has(item.id)}
                    onClick={() => run(item, recall)}
                  >
                    Edit
                  </Button>
                )}
                {insert && (
                  <Button
                    size="compact"
                    disabled={!online || held || working.has(item.id)}
                    title={
                      held
                        ? "Resolve the pending decision first"
                        : "Interrupt the current step and insert this message"
                    }
                    onClick={() => run(item, insert)}
                  >
                    Insert now
                  </Button>
                )}
              </div>
            </div>
          </article>
        ))}
      </div>
    </section>
  );
}
