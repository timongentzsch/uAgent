import type { FileTarget } from "../../shared/file-types.ts";
import type {
  ArtifactPart,
  SessionRef,
  FileOrigin,
} from "../../shared/types.ts";
import { Button } from "../../shared/ui.tsx";
import { nameOf } from "../../shared/file-types.ts";
import { ARTIFACT, artifactSize } from "../../shared/artifact.ts";
import { useApi } from "../../state/api.ts";
import { useResource } from "../../shared/use-resource.ts";
import { PageThumb, usePreviewBase } from "../../shared/page-frame.tsx";

// Drawn from the folder, so it follows what the agent writes. Its cover's box
// is there from the first frame, whatever its requests answer.
export function ArtifactCard({
  session,
  part,
  sequence,
  open,
}: {
  session: SessionRef;
  sequence?: number;
  part: ArtifactPart;
  open: (target: FileTarget) => void;
}) {
  const { api, routes } = useApi();
  const preview = usePreviewBase(session.id);
  const owner = useResource(
    () =>
      api<FileOrigin>(
        routes.fileOrigin(session.id, { path: part.cover, sequence }),
      ),
    [session.id, session.file_origin, part.cover, sequence],
  );
  // Only a file of this conversation's folder as it is now is framed.
  const framed =
    !!owner.value?.current && owner.value.origin === session.file_origin;
  const unavailable =
    (!!owner.value && !framed) || !!owner.error || !!preview.error;
  const show = () => open({ path: part.path, sequence, exact: true });
  const size = artifactSize(part.type, part.count);
  return (
    <figure class="node-file artifact-card">
      <Button
        variant="quiet"
        class="artifact-card-cover"
        aria-label={`Open ${part.path}`}
        onClick={show}
      >
        <PageThumb
          base={framed ? preview.value?.base : undefined}
          path={part.cover}
        />
        {unavailable && <span class="small muted">Preview unavailable</span>}
      </Button>
      <figcaption>
        <strong>{nameOf(part.path.replace(/\/$/, ""))}</strong>
        <span class="muted">
          {ARTIFACT[part.type].name}
          {size && ` · ${size}`}
        </span>
        <Button onClick={show}>Open</Button>
      </figcaption>
    </figure>
  );
}
