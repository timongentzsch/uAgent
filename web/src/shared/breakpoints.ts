// Every width or height at which the layout changes, named once. Stylesheets
// write a name in a query, as `@media (--phone)` or `@container column
// (--narrow-column)`, and the build puts the query in its place: CSS reads no
// custom property in a query. Script reads the same queries.
export const breakpoints = {
  // A phone: the drawer, sheets from the bottom, one pane at a time.
  "--phone": "(max-width: 600px)",
  // A conversation's column too narrow for a time beside each line.
  "--narrow-column": "(max-width: 43rem)",
  // A column wide enough for the side column beside the conversation.
  "--wide-column": "(min-width: 71.5rem)",
  // An app too short for its docked panes.
  "--short": "(max-height: 500px)",
  // An app wide enough to dock the sidebar beside a conversation.
  "--dockable": "(min-width: 44.28rem)",
  // A Files pane too narrow for the list beside the viewer.
  "--narrow-files": "(max-width: 40rem)",
  // A composer wide enough to keep its controls on one row.
  "--roomy-composer": "(min-width: 40rem)",
} as const;

// A stylesheet with each `(--name)` in a query replaced by its query.
export const withBreakpoints = (css: string) =>
  css.replace(/(?<![\w-])\((--[a-z-]+)\)/g, (written, name: string) =>
    name in breakpoints
      ? breakpoints[name as keyof typeof breakpoints]
      : written,
  );
