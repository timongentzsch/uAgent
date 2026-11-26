import { Group, Row } from "../../shared/ui.tsx";
import { TOPICS, type Topic } from "../../shared/settings-topics.ts";

export function SettingsNav({
  topics,
  current,
  select,
  details,
  class: className,
}: {
  topics: Topic[];
  current?: Topic;
  select: (id: Topic) => void;
  details?: Partial<Record<Topic, string>>;
  class?: string;
}) {
  return (
    <nav class={`settings-nav ${className ?? ""}`} aria-label="Settings topics">
      <Group>
        {TOPICS.filter(({ id }) => topics.includes(id)).map(
          ({ id, label, icon: Icon }) => (
            <Row
              key={id}
              icon={<Icon />}
              label={label}
              detail={details?.[id]}
              current={id === current}
              onClick={() => select(id)}
            />
          ),
        )}
      </Group>
    </nav>
  );
}
