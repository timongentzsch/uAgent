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
import { PageThumb } from "../../shared/page-frame.tsx";

// Drawn from the folder, so it follows what the agent writes.
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
  const preview = useResource(async () => {
    const owner = await api<FileOrigin>(
      routes.fileOrigin(session.id, { path: part.cover, sequence }),
    );
    if (!owner.current || owner.origin !== session.file_origin) return;
    const { base } = await api<{ base: string }>(
      routes.preview(session.id),
      {},
    );
    return { base, origin: owner.origin };
  }, [session.id, session.file_origin, part.cover, sequence]);
  const show = () => open({ path: part.path, sequence, exact: true });
  const size = artifactSize(part.type, part.count);
  return (
    <figure class="node-file artifact-card">
      {preview.value && preview.value.origin === session.file_origin && (
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
          {ARTIFACT[part.type].name}
          {size && ` · ${size}`}
        </span>
        <Button onClick={show}>Open</Button>
      </figcaption>
    </figure>
  );
}
