import { useCallback, useEffect, useMemo, useRef, useState } from "react";
import { createRoot } from "react-dom/client";
import { loader } from "@monaco-editor/react";
import { Bot, Code2, Columns2, FolderGit2, SquareTerminal } from "lucide-react";
import {
  agentUrl,
  closeProject as closeProjectRequest,
  createEntry,
  createSession,
  deleteEntry,
  deleteSession as deleteSessionRequest,
  getFiles,
  getModels,
  getPlayground,
  getProjects,
  getSessions,
  getTree,
  openProject as openProjectRequest,
  readFile,
  renameEntry,
  searchWorkspace,
  uploadFile,
  watchPlayground,
  writeFile
} from "./api";
import { ChatPanel } from "./components/ChatPanel";
import { EditorArea } from "./components/EditorArea";
import { Sidebar } from "./components/Sidebar";
import { TerminalLauncher } from "./components/TerminalLauncher";
import { WorkspacePalette } from "./components/WorkspacePalette";
import type {
  ChatEvent,
  ChatItem,
  ApprovalCard,
  ChatStatus,
  FileChange,
  LiveTurn,
  ModelInfo,
  OpenDocument,
  PlaygroundInfo,
  ProjectInfo,
  SearchMatch,
  SessionMeta,
  TodoItem,
  ToolCall,
  UploadAttachment,
  ToolDiff,
  TreeEntry,
  Usage
} from "./types";
import { fileName, isImagePath } from "./utils";
import "./styles.css";

// Monaco is served from the bundled static copy, not a CDN; the workbench
// must run inside an offline personal container.
loader.config({ paths: { vs: "/monaco/vs" } });

type PaletteMode = "files" | "search";
type SplitMode = "editor" | "split" | "ai";
type RevealLocation = { path: string; line: number; column: number; token: number };

const defaultModel = "deepseek-flash";
const defaultEffort = "high";
const STORAGE_KEY = "sparsity.workspace.v1";

type StoredWorkspace = {
  version: 1;
  projects: string[];
  activeProjectId: string | null;
  tabs: Record<string, string[]>;
  activeTabs: Record<string, string | null>;
  sessions: Record<string, string | null>;
  splitMode: SplitMode;
};

function readStoredWorkspace(): StoredWorkspace | null {
  try {
    const raw = localStorage.getItem(STORAGE_KEY);
    if (!raw) return null;
    const value = JSON.parse(raw) as Partial<StoredWorkspace>;
    if (value.version !== 1 || !Array.isArray(value.projects)) return null;
    return {
      version: 1,
      projects: value.projects.filter((path): path is string => typeof path === "string"),
      activeProjectId: typeof value.activeProjectId === "string" ? value.activeProjectId : null,
      tabs: value.tabs && typeof value.tabs === "object" ? value.tabs as Record<string, string[]> : {},
      activeTabs: value.activeTabs && typeof value.activeTabs === "object" ? value.activeTabs as Record<string, string | null> : {},
      sessions: value.sessions && typeof value.sessions === "object" ? value.sessions as Record<string, string | null> : {},
      splitMode: value.splitMode === "editor" || value.splitMode === "ai" || value.splitMode === "split" ? value.splitMode : "split",
    };
  } catch {
    return null;
  }
}

export function App() {
  const storedWorkspace = useRef<StoredWorkspace | null>(readStoredWorkspace());
  const restoredWorkspace = useRef(false);
  const [playground, setPlayground] = useState<PlaygroundInfo | null>(null);
  const [treeEntries, setTreeEntries] = useState<TreeEntry[]>([]);
  const [loadingPlayground, setLoadingPlayground] = useState(true);
  const loadedDirectories = useRef<Set<string>>(new Set([""]));
  const [loadingDirectories, setLoadingDirectories] = useState<Set<string>>(new Set());
  const [projects, setProjects] = useState<ProjectInfo[]>([]);
  const projectsRef = useRef<ProjectInfo[]>(projects);
  projectsRef.current = projects;
  const [activeProjectId, setActiveProjectId] = useState<string | null>(null);
  const activeProjectIdRef = useRef<string | null>(activeProjectId);
  activeProjectIdRef.current = activeProjectId;
  const [documents, setDocuments] = useState<Record<string, OpenDocument[]>>({});
  const documentsRef = useRef<Record<string, OpenDocument[]>>(documents);
  documentsRef.current = documents;
  const [activePaths, setActivePaths] = useState<Record<string, string | null>>({});
  const [selectedPath, setSelectedPath] = useState<string | null>(null);
  const [sessions, setSessions] = useState<Record<string, SessionMeta[]>>({});
  const [activeSessions, setActiveSessions] = useState<Record<string, string | null>>({});
  const [chats, setChats] = useState<Record<string, ChatItem[]>>({});
  const [liveTurns, setLiveTurns] = useState<Record<string, LiveTurn>>({});
  const liveRef = useRef<Record<string, LiveTurn>>(liveTurns);
  liveRef.current = liveTurns;
  const [chatStatuses, setChatStatuses] = useState<Record<string, ChatStatus>>({});
  const [chatStats, setChatStats] = useState<Record<string, ChatStats>>({});
  const [chatPermissions, setChatPermissions] = useState<Record<string, string>>({});
  const [chatLimits, setChatLimits] = useState<Record<string, ChatLimits>>({});
  const [todosByProject, setTodosByProject] = useState<Record<string, TodoItem[]>>({});
  const [attachmentsByProject, setAttachmentsByProject] = useState<Record<string, UploadAttachment[]>>({});
  const [models, setModels] = useState<ModelInfo[]>([]);
  const [model, setModel] = useState(defaultModel);
  const modelRef = useRef(model);
  modelRef.current = model;
  const [effort, setEffort] = useState(defaultEffort);
  const effortRef = useRef(effort);
  effortRef.current = effort;
  const permissionsRef = useRef("workspace");
  const [splitMode, setSplitMode] = useState<SplitMode>("split");
  const [terminalVisible, setTerminalVisible] = useState(false);
  const [paletteMode, setPaletteMode] = useState<PaletteMode | null>(null);
  const [quickOpenEntries, setQuickOpenEntries] = useState<TreeEntry[]>([]);
  const [loadingQuickOpen, setLoadingQuickOpen] = useState(false);
  const [searchResults, setSearchResults] = useState<SearchMatch[]>([]);
  const [searching, setSearching] = useState(false);
  const searchRequest = useRef(0);
  const [revealLocation, setRevealLocation] = useState<RevealLocation | null>(null);
  const [status, setStatus] = useState<string | null>(null);
  const [error, setError] = useState<string | null>(null);
  const agentSocket = useRef<WebSocket | null>(null);
  const nextId = useRef(1);
  const changeHandler = useRef<(changes: FileChange[]) => void>(() => undefined);
  const playgroundRef = useRef<PlaygroundInfo | null>(playground);
  playgroundRef.current = playground;

  const activeProject = useMemo(
    () => projects.find((project) => project.id === activeProjectId) ?? null,
    [activeProjectId, projects]
  );
  const activeSessionId = activeProjectId ? activeSessions[activeProjectId] ?? null : null;
  const activeSessionList = activeProjectId ? sessions[activeProjectId] ?? [] : [];
  const activeDocuments = activeProjectId ? documents[activeProjectId] ?? [] : [];
  const activeDocument = activeDocuments.find((document) => document.path === activePaths[activeProjectId ?? ""]) ?? null;
  const activeItems = activeProjectId ? chats[activeProjectId] ?? [] : [];
  const activeLive = activeProjectId ? liveTurns[activeProjectId] ?? null : null;
  const activeStatus: ChatStatus = activeProjectId ? chatStatuses[activeProjectId] ?? "idle" : "idle";
  const activeStats = activeProjectId ? chatStats[activeProjectId] ?? emptyStats() : emptyStats();
  const activePermissions = activeProjectId ? chatPermissions[activeProjectId] ?? "workspace" : "workspace";
  permissionsRef.current = activePermissions;
  const activeLimits = activeProjectId ? chatLimits[activeProjectId] ?? { maxSteps: 0, maxTurnCost: 1 } : { maxSteps: 0, maxTurnCost: 1 };
  const activeTodos = activeProjectId ? todosByProject[activeProjectId] ?? [] : [];
  const activeAttachments = activeProjectId ? attachmentsByProject[activeProjectId] ?? [] : [];
  const dirtyProjectIds = useMemo(() => new Set(
    Object.entries(documents)
      .filter(([, docs]) => docs.some((document) => document.content !== document.savedContent))
      .map(([id]) => id)
  ), [documents]);
  const runningProjectIds = useMemo(() => new Set(
    Object.entries(chatStatuses)
      .filter(([, value]) => value === "streaming" || value === "approval" || value === "busy")
      .map(([id]) => id)
  ), [chatStatuses]);
  const activePlaygroundPath = activeProject && activeDocument
    ? fullPlaygroundPath(activeProject, activeDocument.path)
    : null;

  useEffect(() => {
    let cancelled = false;
    async function load() {
      try {
        const [playgroundInfo, catalog] = await Promise.all([getPlayground(), getModels()]);
        if (cancelled) return;
        setPlayground(playgroundInfo);
        setModels(catalog.models);
        const fallback = catalog.models.find((candidate) => candidate.id === defaultModel) ?? catalog.models[0];
        if (fallback) {
          setModel(fallback.id);
          const levels = fallback.effort.supported_levels;
          setEffort(levels.includes(defaultEffort) ? defaultEffort : fallback.effort.default_level ?? levels[0] ?? "off");
        }
        const [entries, openProjects] = await Promise.all([getTree(), getProjects()]);
        if (cancelled) return;
        setTreeEntries(entries);
        loadedDirectories.current = new Set([""]);
        const stored = storedWorkspace.current;
        const merged = [...openProjects];
        for (const path of stored?.projects ?? []) {
          if (!path || merged.some((project) => project.path === path)) continue;
          try {
            merged.push(await openProjectRequest(path));
          } catch {
            // The project may have moved or been deleted since the last session.
          }
        }
        setProjects(merged);
        if (stored?.tabs) {
          for (const project of merged) {
            for (const path of stored.tabs[project.id] ?? []) {
              openDocumentFor(project, path);
            }
            const activePath = stored.activeTabs?.[project.id];
            if (activePath) setActivePaths((current) => ({ ...current, [project.id]: activePath }));
          }
        }
        if (stored?.sessions) {
          const values: Record<string, string> = {};
          for (const project of merged) {
            const sessionId = stored.sessions[project.id];
            if (sessionId) values[project.id] = sessionId;
          }
          setActiveSessions((current) => ({ ...current, ...values }));
        }
        if (stored?.splitMode) setSplitMode(stored.splitMode);
        const desired = (stored?.activeProjectId
          ? merged.find((project) => project.id === stored.activeProjectId)
          : null) ?? merged[0] ?? null;
        if (desired) {
          setActiveProjectId(desired.id);
          if (!stored?.tabs?.[desired.id]?.length && desired.kind === "file" && desired.initial_file) {
            openDocumentFor(desired, desired.initial_file);
          }
        } else {
          setActiveProjectId(null);
        }
        restoredWorkspace.current = true;
      } catch (caught) {
        setError(errorMessage(caught));
      } finally {
        if (!cancelled) setLoadingPlayground(false);
      }
    }
    void load();
    return () => { cancelled = true; };
  }, []);

  useEffect(() => {
    if (!playground) return;
    return watchPlayground((changes) => changeHandler.current(changes));
  }, [playground?.root]);

  useEffect(() => {
    if (!restoredWorkspace.current) return;
    const timer = window.setTimeout(() => {
      try {
        const tabs: Record<string, string[]> = {};
        for (const [projectId, docs] of Object.entries(documents)) {
          tabs[projectId] = docs.map((document) => document.path);
        }
        const activeTabs: Record<string, string | null> = {};
        for (const [projectId, path] of Object.entries(activePaths)) {
          if (path) activeTabs[projectId] = path;
        }
        const value: StoredWorkspace = {
          version: 1,
          projects: projects.map((project) => project.path),
          activeProjectId,
          tabs,
          activeTabs,
          sessions: activeSessions,
          splitMode,
        };
        localStorage.setItem(STORAGE_KEY, JSON.stringify(value));
      } catch {
        // Storage can be unavailable or full; the workspace still works.
      }
    }, 250);
    return () => window.clearTimeout(timer);
  }, [projects, activeProjectId, documents, activePaths, activeSessions, splitMode]);

  useEffect(() => {
    const handler = (event: KeyboardEvent) => {
      if ((event.ctrlKey || event.metaKey) && event.key.toLowerCase() === "p" && activeProject) {
        event.preventDefault();
        void showFilePalette();
      }
      if ((event.ctrlKey || event.metaKey) && event.shiftKey && event.key.toLowerCase() === "f") {
        event.preventDefault();
        setPaletteMode("search");
      }
    };
    window.addEventListener("keydown", handler);
    return () => window.removeEventListener("keydown", handler);
  }, [activeProject]);

  useEffect(() => {
    const hasDirty = Object.values(documents).some((docs) => docs.some((document) => document.content !== document.savedContent));
    if (!hasDirty) return;
    const warn = (event: BeforeUnloadEvent) => event.preventDefault();
    window.addEventListener("beforeunload", warn);
    return () => window.removeEventListener("beforeunload", warn);
  }, [documents]);

  useEffect(() => {
    setPaletteMode(null);
    setQuickOpenEntries([]);
    setSearchResults([]);
  }, [activeProjectId]);

  useEffect(() => {
    if (activeProjectId) void ensureSessions(activeProjectId);
  }, [activeProjectId]);

  useEffect(() => {
    const projectId = activeProjectId;
    if (!projectId || !activeSessionId) {
      agentSocket.current?.close();
      agentSocket.current = null;
      return;
    }

    setChatStatuses((current) => ({ ...current, [projectId]: "connecting" }));
    const socket = new WebSocket(agentUrl(projectId, activeSessionId));
    agentSocket.current = socket;
    const patchLive = (updater: (live: LiveTurn) => LiveTurn) => {
      const next = {
        ...liveRef.current,
        [projectId]: updater(liveRef.current[projectId] ?? emptyLive())
      };
      liveRef.current = next;
      setLiveTurns(next);
    };
    const setProjectStatus = (value: ChatStatus) => {
      setChatStatuses((current) => ({ ...current, [projectId]: value }));
    };

    socket.onmessage = (event) => {
      let message: ServerMessage;
      try {
        message = JSON.parse(String(event.data)) as ServerMessage;
      } catch {
        return;
      }
      switch (message.type) {
        case "ready":
          setProjectStatus("idle");
          if (typeof message.model === "string" && message.model) {
            setModel(message.model);
            modelRef.current = message.model;
          }
          if (typeof message.effort === "string" && message.effort) {
            setEffort(message.effort);
            effortRef.current = message.effort;
          }
          if (typeof message.permissions === "string" && message.permissions) {
            setChatPermissions((current) => ({ ...current, [projectId]: message.permissions as string }));
          }
          break;
        case "history": {
          const events = (message.events as ChatEvent[]) ?? [];
          // P0's fake agent has no persisted session. Keep the in-memory
          // conversation when it reconnects; P1 sends real history.
          if (events.length > 0) {
            setChats((current) => ({ ...current, [projectId]: historyToChatItems(events) }));
          }
          const lastTodos = [...events].reverse().find((event) => event.type === "todos");
          setTodosByProject((current) => ({
            ...current,
            [projectId]: lastTodos && lastTodos.type === "todos" ? lastTodos.items : [],
          }));
          break;
        }
        case "turn_start": {
          const nextLive = { ...liveRef.current, [projectId]: emptyLive() };
          liveRef.current = nextLive;
          setLiveTurns(nextLive);
          setProjectStatus("streaming");
          break;
        }
        case "thinking":
          patchLive((live) => ({ ...live, thinking: live.thinking + String(message.text ?? "") }));
          break;
        case "text":
          patchLive((live) => ({ ...live, text: live.text + String(message.text ?? "") }));
          break;
        case "tool_call":
          patchLive((live) => ({
            ...live,
            tools: [
              ...live.tools.filter((tool) => tool.id !== String(message.id)),
              {
                id: String(message.id),
                name: String(message.name ?? ""),
                summary: String(message.summary ?? ""),
                detail: message.detail ? String(message.detail) : undefined,
                diff: message.diff as ToolDiff | undefined
              }
            ]
          }));
          break;
        case "tool_result":
          patchLive((live) => ({
            ...live,
            tools: live.tools.map((tool) => tool.id === String(message.id)
              ? {
                  ...tool,
                  result: {
                    id: String(message.id),
                    text: String(message.text ?? ""),
                    is_error: Boolean(message.is_error),
                    display: String(message.display ?? ""),
                    images: message.images as Array<{ path: string; width?: number; height?: number }> | undefined
                  }
                }
              : tool)
          }));
          break;
        case "step": {
          const calls = (message.tool_calls as ToolCall[] | undefined) ?? [];
          const text = String(message.text ?? "");
          const thinking = String(message.thinking ?? "");
          if (text || thinking || calls.length > 0) {
            setChats((current) => ({
              ...current,
              [projectId]: [
                ...(current[projectId] ?? []),
                {
                  kind: "assistant",
                  id: nextChatId(),
                  thinking,
                  text,
                  tools: calls,
                  usage: message.usage as Usage | undefined,
                  stopReason: typeof message.stop_reason === "string" ? message.stop_reason : undefined
                }
              ]
            }));
          }
          const nextLive = { ...liveRef.current };
          delete nextLive[projectId];
          liveRef.current = nextLive;
          setLiveTurns(nextLive);
          break;
        }
        case "notice":
          setChats((current) => ({
            ...current,
            [projectId]: [
              ...(current[projectId] ?? []),
              {
                kind: "notice",
                id: nextChatId(),
                message: String(message.message ?? ""),
                noticeKind: typeof message.kind === "string" ? message.kind : undefined,
                action: typeof message.prompt === "string"
                  ? {
                      label: typeof message.label === "string" ? message.label : "Continue",
                      prompt: message.prompt
                    }
                  : undefined
              }
            ]
          }));
          break;
        case "loop":
          setChats((current) => ({
            ...current,
            [projectId]: [
              ...(current[projectId] ?? []),
              {
                kind: "loop",
                id: nextChatId(),
                message: String(message.message ?? ""),
                tool: typeof message.tool === "string" ? message.tool : undefined,
                count: typeof message.count === "number" ? message.count : undefined,
                summary: typeof message.summary === "string" ? message.summary : undefined,
                level: typeof message.level === "number" ? message.level : 3
              }
            ]
          }));
          break;
        case "loop_check":
          setChats((current) => ({
            ...current,
            [projectId]: [
              ...(current[projectId] ?? []),
              {
                kind: "loop",
                id: nextChatId(),
                message: String(message.message ?? ""),
                tool: typeof message.tool === "string" ? message.tool : undefined,
                count: typeof message.count === "number" ? message.count : undefined,
                summary: typeof message.summary === "string" ? message.summary : undefined,
                checkId: String(message.id ?? ""),
                level: typeof message.level === "number" ? message.level : (typeof message.count === "number" ? message.count : 5)
              }
            ]
          }));
          setProjectStatus("approval");
          break;
        case "approval":
          patchLive((live) => ({
            ...live,
            approval: { id: String(message.id), card: message.card as ApprovalCard }
          }));
          setProjectStatus("approval");
          break;
        case "turn_end": {
          const turn = liveRef.current[projectId];
          if (turn && (turn.thinking || turn.text || turn.tools.length)) {
            const item: ChatItem = {
              kind: "assistant",
              id: nextChatId(),
              thinking: turn.thinking,
              text: turn.text,
              tools: turn.tools,
              usage: message.usage as Usage | undefined,
              stopReason: message.stop_reason as string | undefined
            };
            setChats((current) => ({ ...current, [projectId]: [...(current[projectId] ?? []), item] }));
          }
          const nextLive = { ...liveRef.current };
          delete nextLive[projectId];
          liveRef.current = nextLive;
          setLiveTurns(nextLive);
          setProjectStatus("idle");
          void refreshSessions(projectId);
          break;
        }
        case "state":
          setChatStats((current) => ({
            ...current,
            [projectId]: {
              cost: Number(message.cost ?? 0),
              turnCost: Number(message.turn_cost ?? 0),
              ctx: Number(message.ctx_tokens ?? 0),
              cacheRead: Number(message.cache_read_input_tokens ?? 0),
              cacheCreation: Number(message.cache_creation_input_tokens ?? 0),
              input: Number(message.input_tokens ?? 0)
            }
          }));
          if (typeof message.permissions === "string" && message.permissions) {
            setChatPermissions((current) => ({ ...current, [projectId]: message.permissions as string }));
          }
          if (typeof message.max_steps === "number" || typeof message.max_turn_cost === "number") {
            setChatLimits((current) => ({
              ...current,
              [projectId]: {
                maxSteps: typeof message.max_steps === "number"
                  ? message.max_steps
                  : current[projectId]?.maxSteps ?? 0,
                maxTurnCost: typeof message.max_turn_cost === "number"
                  ? message.max_turn_cost
                  : current[projectId]?.maxTurnCost ?? 1
              }
            }));
          }
          setChatStatuses((current) => {
            const value = current[projectId];
            if (value === "streaming" || value === "approval") return current;
            return { ...current, [projectId]: message.busy === true ? "busy" : "idle" };
          });
          break;
        case "compaction":
          setChats((current) => ({
            ...current,
            [projectId]: [
              ...(current[projectId] ?? []),
              {
                kind: "compaction",
                id: nextChatId(),
                summary: String(message.summary ?? ""),
                before: Number(message.before_tokens ?? 0),
                after: Number(message.after_tokens ?? 0),
                hardTrimmed: message.hard_trimmed === true
              }
            ]
          }));
          break;
        case "error":
          setChats((current) => ({
            ...current,
            [projectId]: [
              ...(current[projectId] ?? []),
              { kind: "error", id: nextChatId(), message: String(message.message ?? "agent error") }
            ]
          }));
          setProjectStatus("idle");
          break;
        case "todos":
          setTodosByProject((current) => ({ ...current, [projectId]: (message.items as TodoItem[]) ?? [] }));
          break;
        case "switch":
        case "abort":
          break;
        default:
          break;
      }
    };

    socket.onclose = () => {
      if (agentSocket.current === socket) agentSocket.current = null;
      setChatStatuses((current) => ({ ...current, [projectId]: "idle" }));
      setLiveTurns((current) => {
        if (!(projectId in current)) return current;
        const next = { ...current };
        delete next[projectId];
        return next;
      });
    };
    socket.onerror = () => {
      setError("Agent connection failed");
    };

    return () => {
      socket.close();
      if (agentSocket.current === socket) agentSocket.current = null;
    };
  }, [activeProjectId, activeSessionId]);

  function nextChatId() {
    return `chat-${nextId.current++}`;
  }

  async function refreshSessions(projectId: string) {
    try {
      const list = await getSessions(projectId);
      setSessions((current) => ({ ...current, [projectId]: list }));
    } catch {
      // Keep the current list.
    }
  }

  async function ensureSessions(projectId: string) {
    try {
      let list = await getSessions(projectId);
      if (list.length === 0) {
        const created = await createSession(projectId, modelRef.current, effortRef.current);
        list = [created];
      }
      setSessions((current) => ({ ...current, [projectId]: list }));
      setActiveSessions((current) => {
        const value = current[projectId];
        if (value && list.some((session) => session.id === value)) return current;
        return { ...current, [projectId]: list[0].id };
      });
    } catch (caught) {
      setError(errorMessage(caught));
    }
  }

  function selectSession(projectId: string, sessionId: string) {
    if (activeSessions[projectId] === sessionId) return;
    setActiveSessions((current) => ({ ...current, [projectId]: sessionId }));
    setChats((current) => ({ ...current, [projectId]: [] }));
    setLiveTurns((current) => withoutKey(current, projectId));
    setChatStatuses((current) => ({ ...current, [projectId]: "connecting" }));
    setChatStats((current) => ({ ...current, [projectId]: emptyStats() }));
    setTodosByProject((current) => ({ ...current, [projectId]: [] }));
    setAttachmentsByProject((current) => ({ ...current, [projectId]: [] }));
  }

  async function deleteActiveSession() {
    const projectId = activeProjectId;
    const sessionId = projectId ? activeSessions[projectId] : null;
    if (!projectId || !sessionId) return;
    if (!window.confirm("Delete this session?")) return;
    try {
      await deleteSessionRequest(projectId, sessionId);
      const list = await getSessions(projectId);
      setSessions((current) => ({ ...current, [projectId]: list }));
      if (list.length > 0) {
        selectSession(projectId, list[0].id);
      } else {
        setActiveSessions((current) => ({ ...current, [projectId]: null }));
        setChats((current) => ({ ...current, [projectId]: [] }));
        setLiveTurns((current) => withoutKey(current, projectId));
        setChatStats((current) => ({ ...current, [projectId]: emptyStats() }));
        await ensureSessions(projectId);
      }
    } catch (caught) {
      setError(errorMessage(caught));
    }
  }

  async function createNewSession() {
    const projectId = activeProjectId;
    if (!projectId) return;
    try {
      const created = await createSession(projectId, modelRef.current, effortRef.current);
      setSessions((current) => ({ ...current, [projectId]: [created, ...(current[projectId] ?? [])] }));
      selectSession(projectId, created.id);
    } catch (caught) {
      setError(errorMessage(caught));
    }
  }

  function patchProjectList(project: ProjectInfo) {
    setProjects((current) => current.some((candidate) => candidate.id === project.id)
      ? current.map((candidate) => candidate.id === project.id ? project : candidate)
      : [...current, project]);
  }

  function openDocumentFor(project: ProjectInfo, path: string, location?: { line?: number; column?: number }) {
    setActiveProjectId(project.id);
    setActivePaths((current) => ({ ...current, [project.id]: path }));
    if (location?.line) {
      setRevealLocation({ path, line: location.line, column: location.column ?? 1, token: Date.now() });
    }
    const existing = (documentsRef.current[project.id] ?? []).find((document) => document.path === path);
    if (existing) return;

    const image = isImagePath(path);
    const placeholder: OpenDocument = {
      path,
      kind: image ? "image" : "text",
      content: "",
      savedContent: "",
      loading: !image,
      error: null,
      externalChanged: false,
      revision: 0
    };
    setDocuments((current) => ({ ...current, [project.id]: [...(current[project.id] ?? []), placeholder] }));
    if (image) return;

    void (async () => {
      try {
        const file = await readFile(project.id, path);
        setDocuments((current) => ({
          ...current,
          [project.id]: (current[project.id] ?? []).map((document) => document.path === path
            ? { ...document, content: file.content, savedContent: file.content, loading: false, error: null }
            : document)
        }));
      } catch (caught) {
        setDocuments((current) => ({
          ...current,
          [project.id]: (current[project.id] ?? []).map((document) => document.path === path
            ? { ...document, loading: false, error: errorMessage(caught) }
            : document)
        }));
      }
    })();
  }

  async function openAsProject(path: string): Promise<ProjectInfo | null> {
    setError(null);
    try {
      const project = await openProjectRequest(path || ".");
      patchProjectList(project);
      setActiveProjectId(project.id);
      if (project.kind === "file" && project.initial_file) {
        openDocumentFor(project, project.initial_file);
      }
      return project;
    } catch (caught) {
      setError(errorMessage(caught));
      return null;
    }
  }

  async function openTreePath(path: string, kind: "file" | "directory", location?: { line?: number; column?: number }) {
    if (kind === "directory") {
      await openAsProject(path);
      return;
    }
    const containing = findContainingProject(projectsRef.current, activeProjectIdRef.current, path);
    if (containing) {
      const relative = projectRelativePath(containing, path);
      if (relative) {
        openDocumentFor(containing, relative, location);
        return;
      }
    }
    const parent = parentPath(path);
    const project = await openAsProject(parent || ".");
    if (project && project.kind === "folder") {
      const relative = projectRelativePath(project, path);
      if (relative) openDocumentFor(project, relative, location);
    }
  }

  function activateProject(projectId: string) {
    const project = projects.find((candidate) => candidate.id === projectId);
    if (!project) return;
    setActiveProjectId(projectId);
    if (project.kind === "file" && project.initial_file && !(documents[projectId] ?? []).length) {
      openDocumentFor(project, project.initial_file);
    }
  }

  async function closeProject(projectId: string) {
    const project = projects.find((candidate) => candidate.id === projectId);
    if (!project) return;
    const docs = documents[projectId] ?? [];
    if (docs.some((document) => document.content !== document.savedContent) &&
        !window.confirm(`Discard unsaved changes in ${project.name}?`)) {
      return;
    }
    try {
      await closeProjectRequest(projectId);
    } catch (caught) {
      setError(errorMessage(caught));
      return;
    }
    const remaining = projects.filter((candidate) => candidate.id !== projectId);
    setProjects(remaining);
    setDocuments((current) => withoutKey(current, projectId));
    setActivePaths((current) => withoutKey(current, projectId));
    setSessions((current) => withoutKey(current, projectId));
    setActiveSessions((current) => withoutKey(current, projectId));
    setChats((current) => withoutKey(current, projectId));
    setLiveTurns((current) => withoutKey(current, projectId));
    setChatStatuses((current) => withoutKey(current, projectId));
    setChatStats((current) => withoutKey(current, projectId));
    setChatPermissions((current) => withoutKey(current, projectId));
    setChatLimits((current) => withoutKey(current, projectId));
    setTodosByProject((current) => withoutKey(current, projectId));
    setAttachmentsByProject((current) => withoutKey(current, projectId));
    if (activeProjectId === projectId) {
      setActiveProjectId(remaining[0]?.id ?? null);
    }
  }

  async function saveActiveDocument(contentOverride?: string, project: ProjectInfo | null = activeProject) {
    if (!project) return;
    const path = activePaths[project.id];
    const document = (documentsRef.current[project.id] ?? []).find((candidate) => candidate.path === path);
    if (!document || document.kind !== "text" || document.loading) return;
    const content = contentOverride ?? document.content;
    if (content === document.savedContent && !document.externalChanged) {
      setStatus("No changes to save");
      return;
    }
    setStatus("Saving…");
    setError(null);
    try {
      const saved = await writeFile(project.id, document.path, content);
      setDocuments((current) => ({
        ...current,
        [project.id]: (current[project.id] ?? []).map((candidate) => candidate.path === document.path
          ? { ...candidate, content: saved.content, savedContent: saved.content, error: null, externalChanged: false }
          : candidate)
      }));
      setStatus("Saved");
    } catch (caught) {
      setError(errorMessage(caught));
      setStatus(null);
    }
  }

  async function reloadDocument(project: ProjectInfo, path: string) {
    try {
      const file = await readFile(project.id, path);
      setDocuments((current) => ({
        ...current,
        [project.id]: (current[project.id] ?? []).map((candidate) => candidate.path === path
          ? { ...candidate, content: file.content, savedContent: file.content, loading: false, error: null, externalChanged: false }
          : candidate)
      }));
      setStatus("Reloaded from disk");
    } catch (caught) {
      setError(errorMessage(caught));
    }
  }

  function closeDocument(project: ProjectInfo, path: string) {
    const docs = documents[project.id] ?? [];
    const document = docs.find((candidate) => candidate.path === path);
    if (document && document.content !== document.savedContent && !window.confirm(`Discard changes to ${path}?`)) return;
    const index = docs.findIndex((candidate) => candidate.path === path);
    const remaining = docs.filter((candidate) => candidate.path !== path);
    setDocuments((current) => ({ ...current, [project.id]: remaining }));
    if (activePaths[project.id] === path) {
      setActivePaths((current) => ({ ...current, [project.id]: remaining[Math.min(index, remaining.length - 1)]?.path ?? null }));
    }
  }

  function updateActiveDocument(projectId: string, content: string) {
    const path = activePaths[projectId];
    if (!path) return;
    setStatus(null);
    setDocuments((current) => ({
      ...current,
      [projectId]: (current[projectId] ?? []).map((document) => document.path === path ? { ...document, content } : document)
    }));
  }

  async function handleCreate(kind: "file" | "directory", parent: string) {
    const prefix = parent ? `${parent}/` : "";
    const path = window.prompt(`New ${kind === "file" ? "file" : "folder"} path`, prefix);
    if (!path?.trim()) return;
    try {
      await createEntry(null, path.trim(), kind);
      await refreshDirectory(parentPath(path.trim()));
    } catch (caught) {
      setError(errorMessage(caught));
    }
  }

  async function handleRename(path: string) {
    const destination = window.prompt("Rename to", path);
    if (!destination?.trim() || destination.trim() === path) return;
    try {
      await renameEntry(null, path, destination.trim());
      await Promise.all([refreshDirectory(parentPath(path)), refreshDirectory(parentPath(destination.trim()))]);
    } catch (caught) {
      setError(errorMessage(caught));
    }
  }

  async function handleDelete(path: string) {
    if (!window.confirm(`Delete ${path}? This cannot be undone.`)) return;
    try {
      await deleteEntry(null, path);
      const affected = projects.filter((project) => project.path === path || project.path.startsWith(`${path}/`));
      for (const project of affected) {
        try {
          await closeProjectRequest(project.id);
        } catch {
          // The files are gone either way.
        }
      }
      if (affected.length) {
        const ids = new Set(affected.map((project) => project.id));
        const remaining = projects.filter((project) => !ids.has(project.id));
        setProjects(remaining);
        setDocuments((current) => withoutKeys(current, ids));
        setActivePaths((current) => withoutKeys(current, ids));
        setSessions((current) => withoutKeys(current, ids));
        setActiveSessions((current) => withoutKeys(current, ids));
        setChats((current) => withoutKeys(current, ids));
        setLiveTurns((current) => withoutKeys(current, ids));
        setChatStatuses((current) => withoutKeys(current, ids));
        setChatStats((current) => withoutKeys(current, ids));
        setChatPermissions((current) => withoutKeys(current, ids));
        setChatLimits((current) => withoutKeys(current, ids));
        setTodosByProject((current) => withoutKeys(current, ids));
        setAttachmentsByProject((current) => withoutKeys(current, ids));
        if (activeProjectId && ids.has(activeProjectId)) setActiveProjectId(remaining[0]?.id ?? null);
      }
      await refreshDirectory(parentPath(path));
    } catch (caught) {
      setError(errorMessage(caught));
    }
  }

  async function refreshTree() {
    const target = playgroundRef.current;
    if (!target) return;
    try {
      const directories = [...loadedDirectories.current].sort((left, right) => pathDepth(left) - pathDepth(right));
      const results = await Promise.allSettled(directories.map(async (path) => ({ path, entries: await getTree(path) })));
      let entries: TreeEntry[] = [];
      const loaded = new Set<string>();
      for (const result of results) {
        if (result.status !== "fulfilled") continue;
        entries = replaceDirectoryChildren(entries, result.value.path, result.value.entries);
        loaded.add(result.value.path);
      }
      if (!loaded.has("")) throw new Error("Cannot refresh playground root");
      loadedDirectories.current = loaded;
      setTreeEntries(entries);
    } catch (caught) {
      setError(errorMessage(caught));
    }
  }

  async function loadTreeDirectory(path: string) {
    if (loadedDirectories.current.has(path)) return;
    setLoadingDirectories((current) => new Set(current).add(path));
    try {
      const entries = await getTree(path);
      loadedDirectories.current.add(path);
      setTreeEntries((current) => replaceDirectoryChildren(current, path, entries));
    } catch (caught) {
      setError(errorMessage(caught));
    } finally {
      setLoadingDirectories((current) => {
        const next = new Set(current);
        next.delete(path);
        return next;
      });
    }
  }

  async function refreshDirectory(path: string) {
    if (!loadedDirectories.current.has(path)) return;
    try {
      const entries = await getTree(path);
      setTreeEntries((current) => replaceDirectoryChildren(current, path, entries));
    } catch {
      loadedDirectories.current.delete(path);
    }
  }

  async function applyFileChanges(changes: FileChange[]) {
    if (changes.length === 0) return;
    if (changes.some((change) => change.kind === "resync")) {
      await refreshTree();
    } else {
      const parents = new Set(changes
        .filter((change) => change.kind === "created" || change.kind === "removed")
        .map((change) => parentPath(change.path)));
      await Promise.all([...parents].map((path) => refreshDirectory(path)));
    }

    for (const change of changes) {
      if (change.kind === "resync") continue;
      for (const project of projectsRef.current) {
        const relative = projectRelativePath(project, change.path);
        if (!relative) continue;
        const document = (documentsRef.current[project.id] ?? []).find((candidate) => candidate.path === relative);
        if (!document) continue;
        if (change.kind === "removed") {
          setDocuments((current) => ({
            ...current,
            [project.id]: (current[project.id] ?? []).map((candidate) => candidate.path === relative
              ? { ...candidate, error: "This file was removed from disk.", externalChanged: true }
              : candidate)
          }));
          continue;
        }
        if (document.kind === "image") {
          setDocuments((current) => ({
            ...current,
            [project.id]: (current[project.id] ?? []).map((candidate) => candidate.path === relative
              ? { ...candidate, revision: candidate.revision + 1 }
              : candidate)
          }));
          continue;
        }
        if (document.content !== document.savedContent) {
          setDocuments((current) => ({
            ...current,
            [project.id]: (current[project.id] ?? []).map((candidate) => candidate.path === relative
              ? { ...candidate, externalChanged: true }
              : candidate)
          }));
          continue;
        }
        try {
          const file = await readFile(project.id, relative);
          setDocuments((current) => ({
            ...current,
            [project.id]: (current[project.id] ?? []).map((candidate) => {
              if (candidate.path !== relative || candidate.content !== candidate.savedContent) return candidate;
              return { ...candidate, content: file.content, savedContent: file.content, error: null, externalChanged: false };
            })
          }));
        } catch (caught) {
          setDocuments((current) => ({
            ...current,
            [project.id]: (current[project.id] ?? []).map((candidate) => candidate.path === relative
              ? { ...candidate, error: errorMessage(caught), externalChanged: true }
              : candidate)
          }));
        }
      }
    }
  }
  changeHandler.current = (changes) => { void applyFileChanges(changes); };

  const handleSearch = useCallback(async (query: string) => {
    if (!query) {
      setSearchResults([]);
      setSearching(false);
      return;
    }
    const request = ++searchRequest.current;
    setSearching(true);
    try {
      const results = await searchWorkspace(activeProject?.id ?? null, query);
      if (request === searchRequest.current) setSearchResults(results);
    } catch (caught) {
      if (request === searchRequest.current) setError(errorMessage(caught));
    } finally {
      if (request === searchRequest.current) setSearching(false);
    }
  }, [activeProject?.id]);

  async function showFilePalette() {
    setPaletteMode("files");
    if (quickOpenEntries.length > 0) return;
    setLoadingQuickOpen(true);
    try {
      const entries = await getFiles(activeProject?.id ?? null);
      setQuickOpenEntries(entries);
    } catch (caught) {
      setError(errorMessage(caught));
    } finally {
      setLoadingQuickOpen(false);
    }
  }

  function openPaletteLocation(location: { path: string; line?: number; column?: number }) {
    if (activeProject) {
      openDocumentFor(activeProject, location.path, location);
    } else {
      void openTreePath(location.path, "file", location);
    }
    setPaletteMode(null);
  }

  function sendSettings() {
    const socket = agentSocket.current;
    if (socket?.readyState === WebSocket.OPEN) {
      socket.send(JSON.stringify({
        type: "settings",
        model: modelRef.current,
        effort: effortRef.current,
        permissions: permissionsRef.current
      }));
    }
  }

  function changeModel(id: string) {
    const info = models.find((candidate) => candidate.id === id);
    const levels = info?.effort.supported_levels ?? [];
    const nextEffort = levels.includes(effortRef.current)
      ? effortRef.current
      : info?.effort.default_level ?? levels[0] ?? "off";
    setModel(id);
    setEffort(nextEffort);
    modelRef.current = id;
    effortRef.current = nextEffort;
    sendSettings();
  }

  function changeEffort(value: string) {
    setEffort(value);
    effortRef.current = value;
    sendSettings();
  }

  function changePermissions(value: string) {
    const projectId = activeProjectId;
    if (!projectId) return;
    if (value === "auto" &&
        !window.confirm("Auto-approve every tool call, including shell commands? Only use this in a trusted container.")) {
      return;
    }
    setChatPermissions((current) => ({ ...current, [projectId]: value }));
    permissionsRef.current = value;
    sendSettings();
  }

  function changeLimits(maxSteps: number, maxTurnCost: number) {
    const projectId = activeProjectId;
    if (!projectId) return;
    const steps = Math.max(0, Math.trunc(Number.isFinite(maxSteps) ? maxSteps : 0));
    const cost = Math.max(0, Number.isFinite(maxTurnCost) ? maxTurnCost : 0);
    setChatLimits((current) => ({ ...current, [projectId]: { maxSteps: steps, maxTurnCost: cost } }));
    const socket = agentSocket.current;
    if (socket?.readyState === WebSocket.OPEN) {
      socket.send(JSON.stringify({ type: "settings", max_steps: steps, max_turn_cost: cost }));
    }
  }

  async function upload(projectId: string, file: File) {
    const sessionId = activeSessions[projectId];
    if (!sessionId) {
      setError("Open a session before uploading");
      return;
    }
    setStatus(`Uploading ${file.name}…`);
    try {
      const attachment = await uploadFile(projectId, sessionId, file);
      setAttachmentsByProject((current) => ({
        ...current,
        [projectId]: [...(current[projectId] ?? []), attachment],
      }));
      setStatus(null);
    } catch (caught) {
      setError(errorMessage(caught));
      setStatus(null);
    }
  }

  function sendPrompt(text: string) {
    const projectId = activeProjectId;
    const socket = agentSocket.current;
    if (!projectId || !socket || socket.readyState !== WebSocket.OPEN) {
      setError("Agent is not connected");
      return;
    }
    if (activeStatus !== "idle") return;
    const attachments = (attachmentsByProject[projectId] ?? []).flatMap((attachment) => attachment.blocks);
    setChats((current) => ({
      ...current,
      [projectId]: [...(current[projectId] ?? []), { kind: "user", id: nextChatId(), text }]
    }));
    const nextLive = { ...liveRef.current, [projectId]: emptyLive() };
    liveRef.current = nextLive;
    setLiveTurns(nextLive);
    setChatStatuses((current) => ({ ...current, [projectId]: "streaming" }));
    setAttachmentsByProject((current) => ({ ...current, [projectId]: [] }));
    socket.send(JSON.stringify({ type: "prompt", text, attachments }));
  }

  function decide(id: string, action: "allow" | "deny" | "always" | "edit", text?: string) {
    const projectId = activeProjectId;
    const socket = agentSocket.current;
    if (!projectId || !socket || socket.readyState !== WebSocket.OPEN) return;
    setLiveTurns((current) => {
      const live = current[projectId];
      if (!live?.approval || live.approval.id !== id) return current;
      return { ...current, [projectId]: { ...live, approval: { ...live.approval, decision: action } } };
    });
    setChatStatuses((current) => ({ ...current, [projectId]: "streaming" }));
    socket.send(JSON.stringify({ type: "decision", id, action, text }));
  }

  function decideLoop(checkId: string, action: "continue" | "stop") {
    const projectId = activeProjectId;
    const socket = agentSocket.current;
    if (!projectId || !socket || socket.readyState !== WebSocket.OPEN) return;
    setChats((current) => ({
      ...current,
      [projectId]: (current[projectId] ?? []).map((item) =>
        item.kind === "loop" && item.checkId === checkId ? { ...item, decision: action } : item
      )
    }));
    setChatStatuses((current) => ({ ...current, [projectId]: "streaming" }));
    socket.send(JSON.stringify({ type: "loop_decision", id: checkId, action }));
  }

  function interrupt() {
    agentSocket.current?.send(JSON.stringify({ type: "interrupt" }));
  }

  if (loadingPlayground && !playground) {
    return <div className="bootScreen">Starting Sparsity…</div>;
  }

  return (
    <main className="appShell">
      <header className="titleBar">
        <div className="brandMark">S</div>
        <strong>Sparsity</strong>
        <span className="titleWorkspace">
          {playground?.name ?? "playground"}
          {activeProject ? ` · ${activeProject.path}` : ""}
        </span>
        <div className="titleStatus">
          {error ? <span className="globalError" title={error}>{error}</span> : status ? <span>{status}</span> : null}
        </div>
        <div className="titleActions">
          <button
            type="button"
            className={terminalVisible ? "active" : ""}
            onClick={() => setTerminalVisible((value) => !value)}
            title="Terminal (new tab starts at $HOME; refresh kills it)"
          >
            <SquareTerminal size={15} />
          </button>
        </div>
        <div className="viewToggle">
          <button type="button" className={splitMode === "editor" ? "active" : ""} onClick={() => setSplitMode("editor")} title="Editor only">
            <Code2 size={15} />
          </button>
          <button type="button" className={splitMode === "split" ? "active" : ""} onClick={() => setSplitMode("split")} title="Editor and AI">
            <Columns2 size={15} />
          </button>
          <button type="button" className={splitMode === "ai" ? "active" : ""} onClick={() => setSplitMode("ai")} title="AI only">
            <Bot size={15} />
          </button>
        </div>
      </header>

      <div className="workbench">
        <Sidebar
          playground={playground}
          entries={treeEntries}
          loading={loadingPlayground}
          loadingDirectories={loadingDirectories}
          projects={projects}
          activeProjectId={activeProjectId}
          activePath={activePlaygroundPath}
          selectedPath={selectedPath}
          runningProjectIds={runningProjectIds}
          dirtyProjectIds={dirtyProjectIds}
          onSelectPath={setSelectedPath}
          onOpenPath={(path) => void openTreePath(path, "file")}
          onExpand={(path) => void loadTreeDirectory(path)}
          onRefresh={() => void refreshTree()}
          onOpenProject={(path) => void openAsProject(path)}
          onActivateProject={activateProject}
          onCloseProject={(id) => void closeProject(id)}
          onCreate={(kind, parent) => void handleCreate(kind, parent)}
          onRename={(path) => void handleRename(path)}
          onDelete={(path) => void handleDelete(path)}
          onSearch={() => setPaletteMode("search")}
        />

        <section className="mainColumn">
          {activeProject ? (
            <div className={`workbenchBody mode-${splitMode}`}>
              {splitMode !== "ai" ? (
                <EditorArea
                  project={activeProject}
                  documents={activeDocuments}
                  activeDocument={activeDocument}
                  revealLocation={revealLocation}
                  onActivate={(path) => setActivePaths((current) => ({ ...current, [activeProject.id]: path }))}
                  onClose={(path) => closeDocument(activeProject, path)}
                  onChange={(content) => updateActiveDocument(activeProject.id, content)}
                  onSave={(contentOverride) => saveActiveDocument(contentOverride, activeProject)}
                  onReload={(path) => void reloadDocument(activeProject, path)}
                  onOpenPath={(path, line, column) => openDocumentFor(activeProject, path, { line, column })}
                  onQuickOpen={() => void showFilePalette()}
                />
              ) : null}
              {splitMode !== "editor" ? (
                <ChatPanel
                  project={activeProject}
                  models={models}
                  model={model}
                  effort={effort}
                  permissions={activePermissions}
                  maxSteps={activeLimits.maxSteps}
                  maxTurnCost={activeLimits.maxTurnCost}
                  turnCost={activeStats.turnCost}
                  sessions={activeSessionList}
                  activeSessionId={activeSessionId}
                  attachments={activeAttachments}
                  onSelectSession={(id) => activeProject && selectSession(activeProject.id, id)}
                  onNewSession={() => void createNewSession()}
                  onDeleteSession={() => void deleteActiveSession()}
                  onCompact={() => agentSocket.current?.send(JSON.stringify({ type: "compact" }))}
                  onUpload={(file) => activeProject && void upload(activeProject.id, file)}
                  onRemoveAttachment={(index) => setAttachmentsByProject((current) => ({
                    ...current,
                    [activeProjectId ?? ""]: (current[activeProjectId ?? ""] ?? []).filter((_, position) => position !== index),
                  }))}
                  onModelChange={changeModel}
                  onEffortChange={changeEffort}
                  onPermissionsChange={changePermissions}
                  onLimitsChange={changeLimits}
                  onLoopDecision={decideLoop}
                  items={activeItems}
                  live={activeLive}
                  todos={activeTodos}
                  status={activeStatus}
                  cost={activeStats.cost}
                  ctxTokens={activeStats.ctx}
                  cacheRead={activeStats.cacheRead}
                  cacheCreation={activeStats.cacheCreation}
                  inputTokens={activeStats.input}
                  onSend={sendPrompt}
                  onDecision={decide}
                  onInterrupt={interrupt}
                />
              ) : null}
            </div>
          ) : (
            <section className="projectWelcome">
              <div className="welcomeMark"><FolderGit2 size={34} /></div>
              <h1>Open a project</h1>
              <p>Pick a file or folder in the playground on the left, then click <strong>Open as Project</strong>.</p>
            </section>
          )}
        </section>
      </div>

      <TerminalLauncher visible={terminalVisible} onMinimize={() => setTerminalVisible(false)} />

      {paletteMode ? (
        <WorkspacePalette
          mode={paletteMode}
          entries={paletteMode === "files" ? quickOpenEntries : treeEntries}
          results={searchResults}
          searching={paletteMode === "files" ? loadingQuickOpen : searching}
          onSearch={handleSearch}
          onOpen={openPaletteLocation}
          onClose={() => setPaletteMode(null)}
        />
      ) : null}
    </main>
  );
}

type ServerMessage = {
  type: string;
  [key: string]: unknown;
};

function emptyLive(): LiveTurn {
  return { thinking: "", text: "", tools: [] };
}

type ChatLimits = {
  maxSteps: number;
  maxTurnCost: number;
};

type ChatStats = {
  cost: number;
  turnCost: number;
  ctx: number;
  cacheRead: number;
  cacheCreation: number;
  input: number;
};

function emptyStats(): ChatStats {
  return { cost: 0, turnCost: 0, ctx: 0, cacheRead: 0, cacheCreation: 0, input: 0 };
}

function historyToChatItems(events: ChatEvent[]): ChatItem[] {
  const items: ChatItem[] = [];
  for (const event of events) {
    if (event.type === "user") {
      items.push({ kind: "user", id: `history-${items.length}`, text: event.text });
    } else if (event.type === "assistant") {
      const tools: ToolCall[] = (event.tool_calls ?? []).map((call) => ({ ...call, result: call.result }));
      items.push({
        kind: "assistant",
        id: `history-${items.length}`,
        thinking: event.thinking ?? "",
        text: event.text,
        tools,
        usage: event.usage,
        stopReason: event.stop_reason
      });
    } else if (event.type === "compaction") {
      items.push({
        kind: "compaction",
        id: `history-${items.length}`,
        summary: event.summary,
        before: event.before_tokens,
        after: event.after_tokens,
        hardTrimmed: event.hard_trimmed === true
      });
    }
  }
  return items;
}

function fullPlaygroundPath(project: ProjectInfo, path: string) {
  if (project.kind === "file") return project.path;
  if (project.path === ".") return path;
  return `${project.path}/${path}`;
}

function projectRelativePath(project: ProjectInfo, playgroundPath: string): string | null {
  if (project.kind === "file") {
    return playgroundPath === project.path
      ? project.initial_file ?? fileName(project.path)
      : null;
  }
  if (project.path === ".") return playgroundPath;
  if (playgroundPath === project.path) return null;
  const prefix = `${project.path}/`;
  return playgroundPath.startsWith(prefix) ? playgroundPath.slice(prefix.length) : null;
}

function findContainingProject(projects: ProjectInfo[], activeId: string | null, playgroundPath: string) {
  const candidates = projects.filter((project) => projectRelativePath(project, playgroundPath) !== null);
  const active = candidates.find((project) => project.id === activeId);
  if (active) return active;
  return candidates.sort((left, right) => right.path.length - left.path.length)[0] ?? null;
}

function parentPath(path: string) {
  const separator = path.lastIndexOf("/");
  return separator < 0 ? "" : path.slice(0, separator);
}

function pathDepth(path: string) {
  return path ? path.split("/").length : 0;
}

function replaceDirectoryChildren(current: TreeEntry[], parent: string, children: TreeEntry[]) {
  const nextPaths = new Set(children.map((entry) => entry.path));
  const removedRoots = current
    .filter((entry) => parentPath(entry.path) === parent && !nextPaths.has(entry.path))
    .map((entry) => entry.path);
  const entries = new Map(current
    .filter((entry) => !removedRoots.some((root) => entry.path === root || entry.path.startsWith(`${root}/`)))
    .map((entry) => [entry.path, entry]));
  for (const child of children) entries.set(child.path, child);
  return flattenTree(entries);
}

function flattenTree(entries: Map<string, TreeEntry>) {
  const children = new Map<string, TreeEntry[]>();
  for (const entry of entries.values()) {
    const parent = parentPath(entry.path);
    const siblings = children.get(parent) ?? [];
    siblings.push(entry);
    children.set(parent, siblings);
  }
  for (const siblings of children.values()) {
    siblings.sort((left, right) => left.kind === right.kind
      ? left.name.localeCompare(right.name)
      : left.kind === "directory" ? -1 : 1);
  }
  const result: TreeEntry[] = [];
  const visit = (parent: string) => {
    for (const entry of children.get(parent) ?? []) {
      result.push(entry);
      if (entry.kind === "directory") visit(entry.path);
    }
  };
  visit("");
  return result;
}

function withoutKey<T>(input: Record<string, T>, key: string) {
  const next = { ...input };
  delete next[key];
  return next;
}

function withoutKeys<T>(input: Record<string, T>, keys: Set<string>) {
  const next = { ...input };
  for (const key of keys) delete next[key];
  return next;
}

function errorMessage(error: unknown) {
  return error instanceof Error ? error.message : String(error);
}

createRoot(document.getElementById("root")!).render(<App />);
