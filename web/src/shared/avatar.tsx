// Who wrote or is a chat member, at a glance: the name's initials in a
// circle. Decorative beside the name it stands next to.
export function Avatar({ name }: { name: string }) {
  const letters = name
    .split(/[\s_-]+/)
    .filter(Boolean)
    .slice(0, 2)
    .map((part) => part[0]!.toUpperCase())
    .join("");
  return (
    <span class="avatar" aria-hidden="true">
      {letters || "?"}
    </span>
  );
}
