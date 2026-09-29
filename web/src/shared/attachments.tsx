// Files in the conversation look the same wherever they appear: an image is
// a thumbnail that opens the in-app viewer, any other file a card naming its
// type and size that opens or downloads it.
import { createContext, type ComponentChildren } from "preact";
import { useContext, useState } from "preact/hooks";
import { FileText, Pencil } from "lucide-preact";
import {
  Button,
  Deferred,
  DialogHeader,
  Modal,
  Spinner,
  cleanText,
} from "./ui.tsx";
import { bytes } from "./quantities.ts";
import { ZoomSurface } from "./zoom-surface.tsx";

export interface ViewedImage {
  src: string;
  name: string;
  // A composer draft's own file, which an annotated copy replaces.
  draftId?: string;
}

const annotator = () => import("./annotate.tsx");

// Opens an image in the one viewer the app renders.
export const ImageViewer = createContext<(image: ViewedImage) => void>(
  () => {},
);

// A full-bleed viewer: the image at natural size, never upscaled, with its
// own pinch, double-tap and trackpad zoom. Tap around it to close. With
// `annotate`, it can switch to markup and attach the marked-up copy.
export function ImageViewerDialog({
  image,
  close,
  annotate,
}: {
  image: ViewedImage;
  close: () => void;
  annotate?: (file: File, draftId?: string) => Promise<boolean>;
}) {
  const [editing, setEditing] = useState(false);
  // The image's real width, so the zoom level reads as its actual size.
  const [natural, setNatural] = useState<number>();
  return (
    <Modal
      title={image.name}
      close={close}
      layout="panel"
      className="image-view"
      header={!editing}
      actions={
        <>
          {annotate && (
            <Button
              variant="quiet"
              class="with-icon"
              onClick={() => setEditing(true)}
            >
              <Pencil aria-hidden="true" />
              Annotate
            </Button>
          )}
          <a href={image.src} target="_blank" rel="noopener">
            Open
          </a>
          <a href={`${image.src}?download=1`} download={image.name}>
            Download
          </a>
        </>
      }
    >
      {editing && annotate ? (
        <Deferred
          load={annotator}
          ownsDialog
          src={image.src}
          name={image.name}
          cancel={() => setEditing(false)}
          attach={async (file: File) => {
            // A failed upload is reported and leaves the markup open.
            if (await annotate(file, image.draftId)) close();
          }}
          fallback={
            <>
              <DialogHeader title={image.name} />
              <div class="dialog-body">
                <Spinner surface />
              </div>
            </>
          }
        />
      ) : (
        <ZoomSurface
          key={image.src}
          label={image.name}
          dismiss={close}
          natural={natural}
        >
          <img
            src={image.src}
            alt={image.name}
            draggable={false}
            onLoad={(event) => setNatural(event.currentTarget.naturalWidth)}
          />
        </ZoomSurface>
      )}
    </Modal>
  );
}

// "PDF · 1.2 MB": the extension names the type well enough to recognise.
function fileType(name: string, size?: number) {
  const dot = name.lastIndexOf(".");
  const kind = dot > 0 ? name.slice(dot + 1).toUpperCase() : "File";
  return size == null ? kind : `${kind} · ${bytes(size)}`;
}

export function ImageTile({ src, name, draftId }: ViewedImage) {
  const view = useContext(ImageViewer);
  return (
    <Button
      class="attachment-tile"
      title={name}
      aria-label={`View ${name}`}
      onClick={() => view({ src, name, draftId })}
    >
      <img
        src={src}
        alt={name}
        loading="lazy"
        // A display copy the host no longer keeps leaves no broken image.
        onError={(event) =>
          event.currentTarget.parentElement?.setAttribute("hidden", "")
        }
      />
    </Button>
  );
}

function FileCard({
  name,
  size,
  href,
  children,
}: {
  name: string;
  size?: number;
  href?: string;
  children?: ComponentChildren;
}) {
  const body = (
    <>
      <FileText aria-hidden="true" />
      <span>
        <strong>{cleanText(name)}</strong>
        <small>{children ?? fileType(name, size)}</small>
      </span>
    </>
  );
  return href ? (
    <a class="attachment-card" href={href} target="_blank" rel="noopener">
      {body}
    </a>
  ) : (
    <span class="attachment-card">{body}</span>
  );
}

// A message's or tool's files, images first as tiles, then file cards.
export function AttachmentList({
  files,
  href,
}: {
  files: { id: string; name: string; bytes: number; image?: boolean }[];
  href: (id: string) => string;
}) {
  if (!files.length) return null;
  return (
    <div class="attachment-list">
      {files.map((file) =>
        file.image ? (
          <ImageTile key={file.id} src={href(file.id)} name={file.name} />
        ) : (
          <FileCard
            key={file.id}
            name={file.name}
            size={file.bytes}
            href={href(file.id)}
          />
        ),
      )}
    </div>
  );
}
