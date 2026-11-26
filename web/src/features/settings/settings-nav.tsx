import type { ComponentChildren } from "preact";
import { Group, Row } from "../../shared/ui.tsx";
import { TOPICS, type Topic } from "../../shared/settings-topics.ts";

export function SettingsNav({
  topics,
  current,
  select,
  details,
  class: className,
  search,
  children,
}: {
  topics: Topic[];
  current?: Topic;
  select: (id: Topic) => void;
  details?: Partial<Record<Topic, string>>;
  class?: string;
  search: ComponentChildren;
  // Search results, shown in the list's place.
  children?: ComponentChildren;
}) {
  return (
    <nav class={`settings-nav ${className ?? ""}`} aria-label="Settings topics">
      {search}
      {children || (
        <Group>
          {TOPICS.filter(({ id }) => topics.includes(id)).map(
            ({ id, label, icon: Icon }) => (
              <Row
                key={id}
                icon={<Icon />}
                label={label}
                // The open conversation's name: one line, cut.
                detail={
                  details?.[id] && <span class="truncate">{details[id]}</span>
                }
                current={id === current}
                onClick={() => select(id)}
              />
            ),
          )}
        </Group>
      )}
    </nav>
  );
}
