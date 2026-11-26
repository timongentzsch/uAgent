import { createContext, type ComponentChildren } from "preact";
import { useContext, useState } from "preact/hooks";
import { FileText, Paperclip, Pencil, X } from "lucide-preact";
import {
  Button,
  Deferred,
  DialogHeader,
  IconButton,
  Input,
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
  // Opens in markup, and leaving markup closes the viewer.
  edit?: boolean;
}

const annotator = () => import("./annotate.tsx");

export const ImageViewer = createContext<(image: ViewedImage) => void>(
  () => {},
);

// Zooms where it lies, at natural size and never upscaled.
export function ZoomImage({
  src,
  name,
  dismiss,
}: {
  src: string;
  name: string;
  dismiss?: () => void;
}) {
  const [natural, setNatural] = useState<number>();
  return (
    <ZoomSurface label={name} dismiss={dismiss} natural={natural}>
      <img
        src={src}
        alt={name}
        draggable={false}
        onLoad={(event) => setNatural(event.currentTarget.naturalWidth)}
      />
    </ZoomSurface>
  );
}

export function ImageViewerDialog({
  image,
  close,
  annotate,
}: {
  image: ViewedImage;
  close: () => void;
  annotate?: (file: File, draftId?: string) => Promise<boolean>;
}) {
  const [editing, setEditing] = useState(!!image.edit);
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
            <Button variant="quiet" onClick={() => setEditing(true)}>
              <Pencil />
              Annotate
            </Button>
          )}
          <a href={image.src} target="_blank" rel="noopener">
            Open
          </a>
          <a
            href={`${image.src}${image.src.includes("?") ? "&" : "?"}download=1`}
            download={image.name}
          >
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
          cancel={image.edit ? close : () => setEditing(false)}
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
        <ZoomImage
          key={image.src}
          src={image.src}
          name={image.name}
          dismiss={close}
        />
      )}
    </Modal>
  );
}

// Adds files to a message, in the composer and the box a conversation
// starts from.
export function AttachButton({
  disabled,
  add,
}: {
  disabled?: boolean;
  add: (files: File[]) => void;
}) {
  return (
    <label class="file-button icon-button" title="Attach files">
      <Paperclip />
      <Input
        type="file"
        aria-label="Attach files"
        multiple
        disabled={disabled}
        onChange={(event) => {
          add([...(event.currentTarget.files || [])]);
          event.currentTarget.value = "";
        }}
      />
    </label>
  );
}

// Files pasted on their own. Spreadsheets and web pages put a rendered image
// beside the text; the text is what was copied.
export function pastedFiles(event: ClipboardEvent) {
  const files = [...(event.clipboardData?.files || [])];
  if (!files.length || event.clipboardData?.getData("text/plain")) return [];
  event.preventDefault();
  return files;
}

// A file going with a message: its picture or a clip, what names it, and a
// way to take it off.
export function FileChip({
  name,
  busy,
  tile,
  remove,
  children,
}: {
  name: string;
  busy?: boolean;
  tile?: ComponentChildren;
  remove?: () => void;
  children: ComponentChildren;
}) {
  return (
    <div class="chip chip-outline file-chip" aria-busy={busy || undefined}>
      {tile || <Paperclip />}
      <span title={name}>{children}</span>
      {remove && (
        <IconButton label={`Remove ${name}`} onClick={remove}>
          <X />
        </IconButton>
      )}
    </div>
  );
}

// "PDF · 1.2 MB": the extension names the type well enough to recognise.
function fileType(name: string, size?: number) {
  const dot = name.lastIndexOf(".");
  const kind = dot > 0 ? name.slice(dot + 1).toUpperCase() : "File";
  return size == null ? kind : `${kind} · ${bytes(size)}`;
}

// With its size, a picture's box has its shape before it loads.
export function ImageTile({
  src,
  name,
  draftId,
  width,
  height,
}: ViewedImage & { width?: number; height?: number }) {
  const view = useContext(ImageViewer);
  const sized = !!width && !!height;
  return (
    <Button
      class="attachment-tile"
      title={name}
      aria-label={`View ${name}`}
      onClick={() => view({ src, name, draftId })}
      style={sized ? { "--ratio": width / height } : undefined}
    >
      <img
        src={src}
        alt={name}
        width={sized ? width : undefined}
        height={sized ? height : undefined}
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
      <FileText />
      <span>
        <strong class="truncate">{cleanText(name)}</strong>
        <small>{children ?? fileType(name, size)}</small>
      </span>
    </>
  );
  return href ? (
    <a class="card attachment-card" href={href} target="_blank" rel="noopener">
      {body}
    </a>
  ) : (
    <span class="card attachment-card">{body}</span>
  );
}

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
