import type { ArtifactPart, FileTarget } from "../../shared/types.ts";
import { Button } from "../../shared/ui.tsx";
import { nameOf } from "../../shared/file-types.ts";
import { ARTIFACT, artifactSize } from "../../shared/artifact.ts";
import { PageThumb, usePreviewBase } from "../../shared/page-frame.tsx";

// Drawn from the folder, so it follows what the agent writes.
export function ArtifactCard({
  session,
  part,
  open,
}: {
  session: string;
  part: ArtifactPart;
  open: (target: FileTarget) => void;
}) {
  const preview = usePreviewBase(session);
  const show = () => open({ path: part.path });
  const size = artifactSize(part.type, part.count);
  return (
    <figure class="node-file artifact-card">
      {preview.value && (
        <Button
          variant="quiet"
          class="artifact-card-cover"
          aria-label={`Open ${part.path}`}
          onClick={show}
        >
          <PageThumb base={preview.value.base} path={part.cover} />
        </Button>
      )}
      <figcaption>
        <strong>{nameOf(part.path.replace(/\/$/, ""))}</strong>
        <span class="muted">
          {ARTIFACT[part.type]}
          {size && ` · ${size}`}
        </span>
        <Button onClick={show}>Open</Button>
      </figcaption>
    </figure>
  );
}
