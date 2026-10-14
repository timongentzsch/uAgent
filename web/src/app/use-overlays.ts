import { useCallback, useEffect, useState } from "preact/hooks";
import type { AppModal, InspectorTarget } from "../shared/types.ts";
import type { ViewedImage } from "../shared/attachments.tsx";

// What opens over the page: the conversations drawer, the board sheet, the
// app's dialogs, the palette or shortcuts sheet, the image viewer, the
// inspector and the /btw card.
export function useOverlays(compact: boolean) {
  const [drawer, setDrawer] = useState(false);
  // On a phone a panel's board slides in from the right on demand
  // instead of taking the top of its chat.
  const [boardOpen, setBoardOpen] = useState(false);
  const [modal, setModal] = useState<AppModal | null>(null);
  // The command palette (Mod+K) or the shortcuts sheet (?).
  const [overlay, setOverlay] = useState<"palette" | "shortcuts" | null>(null);
  // The one image viewer every tile opens.
  const [viewed, setViewed] = useState<ViewedImage | null>(null);
  const [side, setSide] = useState<{
    question: string;
    answer?: string;
  } | null>(null);
  const [inspector, setInspector] = useState<InspectorTarget | null>(null);
  // The drawer belongs to the compact layout; a wider window drops it.
  useEffect(() => setDrawer(false), [compact]);
  const closeSide = useCallback(() => setSide(null), []);
  const open = useCallback((value: AppModal) => {
    setDrawer(false);
    setModal(value);
  }, []);
  const openPalette = useCallback(() => {
    setDrawer(false);
    setOverlay("palette");
  }, []);
  return {
    drawer,
    setDrawer,
    boardOpen,
    setBoardOpen,
    modal,
    setModal,
    overlay,
    setOverlay,
    viewed,
    setViewed,
    side,
    setSide,
    closeSide,
    inspector,
    setInspector,
    open,
    openPalette,
  };
}
