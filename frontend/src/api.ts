import type {
  ChatEvent,
  FileChange,
  ModelCatalog,
  PlaygroundInfo,
  ProjectInfo,
  SearchMatch,
  SessionMeta,
  TextFile,
  TreeEntry,
  UploadAttachment
} from "./types";

export async function getPlayground() {
  return fetchJson<PlaygroundInfo>("/api/playground");
}

export async function getTree(path = "") {
  const params = new URLSearchParams();
  if (path) params.set("path", path);
  const suffix = params.toString();
  return fetchJson<TreeEntry[]>(`/api/tree${suffix ? `?${suffix}` : ""}`);
}

export async function getFiles(projectId?: string | null) {
  const params = new URLSearchParams();
  if (projectId) params.set("project", projectId);
  const suffix = params.toString();
  return fetchJson<TreeEntry[]>(`/api/files${suffix ? `?${suffix}` : ""}`);
}

export async function getProjects() {
  return fetchJson<ProjectInfo[]>("/api/projects");
}

export async function openProject(path: string) {
  return fetchJson<ProjectInfo>("/api/projects/open", {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({ path })
  });
}

export async function closeProject(projectId: string) {
  const response = await fetch(`/api/projects/${encodeURIComponent(projectId)}`, { method: "DELETE" });
  if (!response.ok) throw new Error(await readError(response));
}

export async function readFile(projectId: string | null, path: string) {
  return fetchJson<TextFile>("/api/file/read", {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify(projectBody(projectId, { path }))
  });
}

export async function writeFile(projectId: string | null, path: string, content: string) {
  return fetchJson<TextFile>("/api/file/write", {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify(projectBody(projectId, { path, content }))
  });
}

export async function createEntry(projectId: string | null, path: string, kind: "file" | "directory") {
  return fetchEmpty("/api/file/create", projectBody(projectId, { path, kind }));
}

export async function renameEntry(projectId: string | null, path: string, destination: string) {
  return fetchEmpty("/api/file/rename", projectBody(projectId, { path, destination }));
}

export async function deleteEntry(projectId: string | null, path: string) {
  return fetchEmpty("/api/file/delete", projectBody(projectId, { path }));
}

export async function searchWorkspace(projectId: string | null, query: string, caseSensitive = false) {
  return fetchJson<SearchMatch[]>("/api/search", {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify(projectBody(projectId, { query, case_sensitive: caseSensitive }))
  });
}

export async function getSessions(projectId: string) {
  return fetchJson<SessionMeta[]>(`/api/projects/${encodeURIComponent(projectId)}/sessions`);
}

export async function deleteSession(projectId: string, sessionId: string) {
  return fetchJson<{ ok: boolean }>(`/api/projects/${encodeURIComponent(projectId)}/sessions/${encodeURIComponent(sessionId)}`, { method: "DELETE" });
}

export async function createSession(projectId: string, model: string, effort: string) {
  return fetchJson<SessionMeta>(`/api/projects/${encodeURIComponent(projectId)}/sessions`, {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({ model, effort })
  });
}

export async function uploadFile(projectId: string, sessionId: string, file: File) {
  const params = new URLSearchParams({ project: projectId, session: sessionId, name: file.name });
  const response = await fetch(`/api/upload?${params.toString()}`, { method: "POST", body: file });
  if (!response.ok) throw new Error(await readError(response));
  return (await response.json()) as UploadAttachment;
}

export async function getModels() {
  return fetchJson<ModelCatalog>("/api/models");
}

export async function refreshModels() {
  return fetchJson<ModelCatalog>("/api/models/refresh", { method: "POST" });
}

export function watchPlayground(onChanges: (changes: FileChange[]) => void) {
  const protocol = window.location.protocol === "https:" ? "wss:" : "ws:";
  const url = `${protocol}//${window.location.host}/api/watch`;
  let disposed = false;
  let socket: WebSocket | null = null;
  let reconnectTimer: number | null = null;
  const connect = () => {
    if (disposed) return;
    socket = new WebSocket(url);
    socket.addEventListener("message", (event) => {
      try {
        const message = JSON.parse(String(event.data)) as { type?: string; changes?: FileChange[] };
        if (message.type === "changes" && Array.isArray(message.changes)) onChanges(message.changes);
      } catch {
        // Ignore malformed watcher messages and wait for the next snapshot.
      }
    });
    socket.addEventListener("close", () => {
      if (!disposed) reconnectTimer = window.setTimeout(connect, 2500);
    });
  };
  connect();
  return () => {
    disposed = true;
    if (reconnectTimer != null) window.clearTimeout(reconnectTimer);
    socket?.close();
  };
}

export function agentUrl(projectId: string, sessionId: string | null) {
  const protocol = window.location.protocol === "https:" ? "wss:" : "ws:";
  const params = new URLSearchParams({ project: projectId });
  if (sessionId) params.set("session", sessionId);
  return `${protocol}//${window.location.host}/api/agent?${params.toString()}`;
}

export function terminalUrl(cwd?: string) {
  const protocol = window.location.protocol === "https:" ? "wss:" : "ws:";
  const params = new URLSearchParams();
  if (cwd) params.set("cwd", cwd);
  const suffix = params.toString();
  return `${protocol}//${window.location.host}/api/terminal${suffix ? `?${suffix}` : ""}`;
}

export function imageUrl(projectId: string | null, path: string) {
  const params = new URLSearchParams();
  if (projectId) params.set("project", projectId);
  params.set("path", path);
  return `/api/file/raw?${params.toString()}`;
}

export function fileBody(projectId: string | null, values: Record<string, unknown>) {
  return projectBody(projectId, values);
}

function projectBody(projectId: string | null, values: Record<string, unknown>) {
  return projectId ? { project: projectId, ...values } : values;
}

async function fetchJson<T>(input: RequestInfo | URL, init?: RequestInit) {
  const response = await fetch(input, init);
  if (!response.ok) throw new Error(await readError(response));
  return (await response.json()) as T;
}

async function fetchEmpty(input: RequestInfo | URL, body: unknown) {
  const response = await fetch(input, {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify(body)
  });
  if (!response.ok) throw new Error(await readError(response));
}

async function readError(response: Response) {
  const text = await response.text();
  try {
    return (JSON.parse(text) as { error?: string }).error ?? text;
  } catch {
    return text || `${response.status} ${response.statusText}`;
  }
}

export type { ChatEvent };
