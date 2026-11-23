export type PlaygroundInfo = {
  name: string;
  root: string;
  kind: "folder";
};

export type ProjectInfo = {
  id: string;
  path: string;
  name: string;
  kind: "folder" | "file";
  root: string;
  initial_file: string | null;
};

export type TreeEntry = {
  path: string;
  name: string;
  kind: "file" | "directory";
  depth: number;
};

export type TextFile = {
  path: string;
  content: string;
  size: number;
};

export type OpenDocument = {
  path: string;
  kind: "text" | "image";
  content: string;
  savedContent: string;
  loading: boolean;
  error: string | null;
  externalChanged: boolean;
  revision: number;
};

export type SearchMatch = {
  path: string;
  line: number;
  column: number;
  preview: string;
};

export type FileChange = {
  kind: "created" | "modified" | "removed" | "resync";
  path: string;
};

export type TodoItem = { content: string; status: string };

export type UploadAttachment = {
  kind: "image" | "text" | "pdf";
  name: string;
  preview: string;
  blocks: unknown[];
};

export type SessionMeta = {
  id: string;
  title: string;
  messages: number;
  updated: number;
};

export type ModelInfo = {
  id: string;
  name: string;
  owner?: string;
  context_window: number;
  max_output_tokens: number;
  input_modalities: string[];
  output_modalities?: string[];
  effort: {
    supported_levels: string[];
    default_level?: string;
  };
};

export type ModelCatalog = {
  models: ModelInfo[];
  source: "live" | "cache" | "fallback";
  fetched_at: string;
};

export type ToolDiff = {
  path: string;
  before: string;
  after: string;
  added: number;
  removed: number;
  truncated: boolean;
};

export type ToolCall = {
  id: string;
  name: string;
  summary: string;
  detail?: string;
  diff?: ToolDiff;
  result?: ToolResult;
};

export type ToolResult = {
  id: string;
  text: string;
  is_error: boolean;
  display: string;
  images?: Array<{ path: string; width?: number; height?: number }>;
};

export type ApprovalCard = {
  tool: string;
  summary: string;
  detail?: string;
  editable?: string | null;
  danger?: string | null;
  outside_project?: boolean;
  diff?: ToolDiff;
};

export type ChatEvent =
  | { type: "user"; id?: string; text: string; attachments?: unknown[] }
  | {
      type: "assistant";
      id?: string;
      thinking?: string;
      text: string;
      tool_calls?: ToolCall[];
      usage?: Usage;
      stop_reason?: string;
    }
  | { type: "tool_result"; id: string; tool_use_id?: string; text: string; is_error: boolean; display: string; images?: ToolResult["images"] }
  | { type: "todos"; items: Array<{ content: string; status: string }> }
  | { type: "compaction"; summary: string; before_tokens: number; after_tokens: number }
  | { type: "switch"; model: string; effort: string }
  | { type: "abort"; reason: string };

export type Usage = {
  input_tokens: number;
  output_tokens: number;
  cache_read_input_tokens: number;
  cache_creation_input_tokens: number;
};

export type ChatItem =
  | { kind: "user"; id: string; text: string }
  | {
      kind: "assistant";
      id: string;
      thinking: string;
      text: string;
      tools: ToolCall[];
      usage?: Usage;
      stopReason?: string;
    }
  | { kind: "tool"; id: string; call: ToolCall }
  | { kind: "approval"; id: string; call: ToolCall; card: ApprovalCard; decision?: string }
  | { kind: "compaction"; id: string; summary: string; before: number; after: number }
  | { kind: "notice"; id: string; message: string; action?: { label: string; prompt: string } }
  | { kind: "loop"; id: string; message: string; tool?: string; count?: number; summary?: string; level?: number; checkId?: string; decision?: "continue" | "stop" }
  | { kind: "error"; id: string; message: string };

export type LiveTurn = {
  thinking: string;
  text: string;
  tools: ToolCall[];
  approval?: { id: string; card: ApprovalCard; decision?: string };
  usage?: Usage;
  stopReason?: string;
};

export type ChatStatus = "connecting" | "idle" | "streaming" | "approval" | "busy";
