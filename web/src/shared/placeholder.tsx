import { createContext, type ComponentChildren } from "preact";
import { useContext } from "preact/hooks";

// A loading state is the loaded view itself, drawn from sample data inside
// <Placeholder>: primitives draw their data text as placeholder bars and the
// subtree is inert, so a screen and its loading state are one component tree
// and cannot drift apart.
const Drawing = createContext(false);

export const usePlaceholder = () => useContext(Drawing);

export function Placeholder({
  label,
  when = true,
  children,
}: {
  // Announced once for the whole surface; the drawing itself is hidden.
  label: string;
  // False renders the children as they are: a view passes its loading
  // condition and wraps its markup once.
  when?: boolean;
  children: ComponentChildren;
}) {
  if (!when) return <>{children}</>;
  return (
    <Drawing.Provider value={true}>
      <span class="sr-only" role="status" aria-busy="true" aria-label={label}>
        {label}
      </span>
      <div class="placeholder" aria-hidden="true" inert>
        {children}
      </div>
    </Drawing.Provider>
  );
}

// Data text: itself when loaded, a bar of the same length inside a
// Placeholder. Primitives wrap their data slots; views wrap data they render
// in their own markup.
export function DataText({ children }: { children: ComponentChildren }) {
  return usePlaceholder() ? (
    <span class="text-skeleton">{children}</span>
  ) : (
    <>{children}</>
  );
}
