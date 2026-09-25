// Files in the conversation look the same wherever they appear: an image is
// a thumbnail that opens the in-app viewer, any other file a card naming its
// type and size that opens or downloads it.
import { createContext, type ComponentChildren } from "preact";
import { useContext } from "preact/hooks";
import { FileText } from "lucide-preact";
import { Modal, cleanText } from "./ui.tsx";
import { bytes } from "./quantities.ts";

export interface ViewedImage {
  src: string;
  name: string;
}

// Opens an image in the one viewer the app renders.
export const ImageViewer = createContext<(image: ViewedImage) => void>(
  () => {},
);

// Fits the image to the screen; the browser's own pinch zoom still works.
export function ImageViewerDialog({
  image,
  close,
}: {
  image: ViewedImage;
  close: () => void;
}) {
  return (
    <Modal
      title={image.name}
      close={close}
      size="wide"
      layout="panel"
      className="image-view"
      actions={
        <>
          <a href={image.src} target="_blank" rel="noopener">
            Open
          </a>
          <a href={`${image.src}?download=1`} download={image.name}>
            Download
          </a>
        </>
      }
    >
      <img src={image.src} alt={image.name} onClick={close} />
    </Modal>
  );
}

// "PDF · 1.2 MB": the extension names the type well enough to recognise.
export function fileType(name: string, size?: number) {
  const dot = name.lastIndexOf(".");
  const kind = dot > 0 ? name.slice(dot + 1).toUpperCase() : "File";
  return size == null ? kind : `${kind} · ${bytes(size)}`;
}

export function ImageTile({ src, name }: ViewedImage) {
  const view = useContext(ImageViewer);
  return (
    <button
      type="button"
      class="attachment-tile"
      title={name}
      aria-label={`View ${name}`}
      onClick={() => view({ src, name })}
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
    </button>
  );
}

export function FileCard({
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
