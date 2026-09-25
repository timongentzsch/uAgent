import { createContext } from "preact";
import type { Activity } from "../shared/types.ts";

// The session's running work, so a transcript row can show the live state of
// the command or agent its call started.
export const LiveActivities = createContext<Activity[]>([]);
