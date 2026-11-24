import { useEffect, useMemo, useRef, useState } from "react";
import { Bot, CircleStop, FileText, Paperclip, Send, Sparkles, Trash2, User, Wrench } from "lucide-react";
import ReactMarkdown from "react-markdown";
import remarkGfm from "remark-gfm";
import type {
  ApprovalCard,
  ChatItem,
  ChatStatus,
  LiveTurn,
  ModelInfo,
  ProjectInfo,
  SessionMeta,
  TodoItem,
  ToolCall,
  UploadAttachment,
  ToolDiff
} from "../types";

type ChatPanelProps = {
  project: ProjectInfo | null;
  models: ModelInfo[];
  model: string;
  effort: string;
  permissions: string;
  maxSteps: number;
  maxTurnCost: number;
  turnCost: number;
  sessions: SessionMeta[];
  activeSessionId: string | null;
  attachments: UploadAttachment[];
  onSelectSession: (id: string) => void;
  onNewSession: () => void;
  onDeleteSession: () => void;
  onCompact: () => void;
  onUpload: (file: File) => void;
  onRemoveAttachment: (index: number) => void;
  onModelChange: (id: string) => void;
  onEffortChange: (effort: string) => void;
  onPermissionsChange: (permissions: string) => void;
  onLimitsChange: (maxSteps: number, maxTurnCost: number) => void;
  onLoopDecision: (checkId: string, action: "continue" | "stop") => void;
  items: ChatItem[];
  live: LiveTurn | null;
  todos: TodoItem[];
  status: ChatStatus;
  cost: number;
  ctxTokens: number;
  cacheRead: number;
  cacheCreation: number;
  inputTokens: number;
  onSend: (text: string) => void;
  onDecision: (id: string, action: "allow" | "deny" | "always" | "edit", text?: string) => void;
  onInterrupt: () => void;
};

export function ChatPanel({
  project,
  models,
  model,
  effort,
  permissions,
  maxSteps,
  maxTurnCost,
  turnCost,
  sessions,
  activeSessionId,
  attachments,
  onSelectSession,
  onNewSession,
  onDeleteSession,
  onCompact,
  onUpload,
  onRemoveAttachment,
  onModelChange,
  onEffortChange,
  onPermissionsChange,
  onLimitsChange,
  onLoopDecision,
  items,
  live,
  todos,
  status,
  cost,
  ctxTokens,
  cacheRead,
  cacheCreation,
  inputTokens,
  onSend,
  onDecision,
  onInterrupt
}: ChatPanelProps) {
  const [text, setText] = useState("");
  const [stepsDraft, setStepsDraft] = useState(maxSteps);
  const [costDraft, setCostDraft] = useState(maxTurnCost);
  const fileRef = useRef<HTMLInputElement | null>(null);
  const scrollRef = useRef<HTMLDivElement | null>(null);
  const composing = useRef(false);
  const compositionEndedAt = useRef(0);
  const currentModel = useMemo(
    () => models.find((candidate) => candidate.id === model) ?? models[0] ?? null,
    [model, models]
  );
  const effortOptions = currentModel ? ["off", ...currentModel.effort.supported_levels] : ["off", "low", "high", "max"];
  const busy = status === "streaming" || status === "approval" || status === "busy";
  const connecting = status === "connecting";
  const blocked = busy || connecting;
  const changed = new Set<string>();
  const collect = (tool: ToolCall) => {
    if ((tool.name === "write" || tool.name === "edit") && tool.result && !tool.result.is_error) {
      changed.add(tool.summary);
    }
  };
  for (const item of items) {
    if (item.kind === "tool") collect(item.call);
    else if (item.kind === "assistant") for (const tool of item.tools) collect(tool);
  }
  if (live) for (const tool of live.tools) collect(tool);

  useEffect(() => {
    setStepsDraft(maxSteps);
    setCostDraft(maxTurnCost);
  }, [maxSteps, maxTurnCost]);

  useEffect(() => {
    const node = scrollRef.current;
    if (!node) return;
    node.scrollTop = node.scrollHeight;
  }, [items, live, status]);

  if (!project) {
    return (
      <section className="chatPanel chatEmpty">
        <div className="chatEmptyMark"><Bot size={34} /></div>
        <h2>DeepSeek in the loop</h2>
        <p>Select a project on the left to start a conversation about it.</p>
      </section>
    );
  }

  function submit() {
    const value = text.trim();
    if (!value || busy) return;
    setText("");
    onSend(value);
  }

  return (
    <section className="chatPanel">
      <header className="chatHeader">
        <div className="chatTitle">
          <span className="chatProjectName">{project.name}</span>
          <small>{project.path}</small>
        </div>
        <div className="chatControls">
          <select
            value={model}
            onChange={(event) => onModelChange(event.target.value)}
            title="Model"
            disabled={blocked}
          >
            {models.length === 0 ? <option value={model}>{model}</option> : null}
            {models.map((candidate) => (
              <option key={candidate.id} value={candidate.id}>{candidate.name}</option>
            ))}
          </select>
          <select
            value={effort}
            onChange={(event) => onEffortChange(event.target.value)}
            title="Thinking effort"
            disabled={blocked}
          >
            {effortOptions.map((option) => <option key={option} value={option}>{option}</option>)}
          </select>
          <select
            value={permissions}
            onChange={(event) => onPermissionsChange(event.target.value)}
            title="Approval mode: Ask every write/edit/bash, auto-approve project edits and safe read-only commands in Workspace, auto-approve everything in Auto, or deny writes and shell commands in Read only."
            disabled={blocked}
          >
            <option value="ask">Ask</option>
            <option value="workspace">Workspace</option>
            <option value="auto">Auto</option>
            <option value="readonly">Read only</option>
          </select>
          <span className={`chatStatus ${status}`}>{statusLabel(status)}</span>
        </div>
      </header>

      <div className="chatSessionBar">
        <select
          value={activeSessionId ?? ""}
          onChange={(event) => onSelectSession(event.target.value)}
          disabled={busy || sessions.length === 0}
          title="Session"
        >
          {sessions.length === 0 ? <option value="">New chat</option> : null}
          {sessions.map((session) => (
            <option key={session.id} value={session.id}>{session.title || "(untitled)"}</option>
          ))}
        </select>
        <button type="button" onClick={onNewSession} disabled={connecting}>+ New chat</button>
        <button type="button" onClick={onDeleteSession} disabled={blocked || !activeSessionId} title="Delete session"><Trash2 size={12} /></button>
        <button type="button" onClick={onCompact} disabled={blocked || items.length === 0} title="Compact history">Compact</button>
        <details className="limitsPanel">
          <summary title="Session limits">Limits</summary>
          <div className="limitsBody">
            <label>
              Max steps
              <input type="number" min={0} step={1} value={stepsDraft} disabled={blocked}
                onChange={(event) => setStepsDraft(Number(event.target.value))} />
            </label>
            <small>0 = unlimited</small>
            <label>
              Max turn cost ($)
              <input type="number" min={0} step={0.5} value={costDraft} disabled={blocked}
                onChange={(event) => setCostDraft(Number(event.target.value))} />
            </label>
            <small>0 = no hard cost limit</small>
            <button type="button" disabled={blocked} onClick={() => onLimitsChange(stepsDraft, costDraft)}>Apply</button>
          </div>
        </details>
      </div>

      <div className="chatTimeline" ref={scrollRef}>
        {items.length === 0 && !live ? (
          <div className="chatHint">
            <Sparkles size={16} />
            <p>Ask about this project. Upload an image, text or PDF, search the web, or ask for a change.</p>
          </div>
        ) : null}
        {items.map((item) => <ChatItemView key={item.id} item={item} onSend={onSend} onLoopDecision={onLoopDecision} />)}
        {live ? <LiveView live={live} status={status} onDecision={onDecision} /> : null}
      </div>

      {changed.size > 0 ? (
        <div className="changeBar" title={Array.from(changed).join("\n")}>
          <FileText size={11} /> Changed {changed.size} {changed.size === 1 ? "file" : "files"}: {Array.from(changed).join(", ")}
        </div>
      ) : null}

      {todos.length > 0 ? (
        <details className="todoPanel" open>
          <summary>
            Tasks {todos.filter((item) => item.status === "completed").length}/{todos.length}
          </summary>
          <ul>
            {todos.map((item, index) => (
              <li key={index} className={item.status}>
                <span>{item.status === "completed" ? "✓" : item.status === "in_progress" ? "◐" : "○"}</span>
                {item.content}
              </li>
            ))}
          </ul>
        </details>
      ) : null}

      <footer className="composer">
        <div className="composerMeta">
          <span>ctx {formatTokens(ctxTokens)}</span>
          {cacheRead > 0 ? (
            <span title={`cache read ${cacheRead} · cache creation ${cacheCreation} · input ${inputTokens}`}>
              cache {formatTokens(cacheRead)}{inputTokens + cacheRead + cacheCreation > 0 ? ` · ${Math.round((cacheRead / (inputTokens + cacheRead + cacheCreation)) * 100)}%` : ""}
            </span>
          ) : null}
          <span title="This turn's cost / hard budget">turn ${turnCost.toFixed(2)} / {maxTurnCost > 0 ? `$${maxTurnCost.toFixed(2)}` : "off"}</span>
          <span>${cost.toFixed(4)}</span>
          <span>{currentModel?.id ?? model}</span>
        </div>
        {attachments.length > 0 ? (
          <div className="attachmentChips">
            {attachments.map((attachment, index) => (
              <span key={`${attachment.name}-${index}`} className="attachmentChip" title={attachment.preview}>
                <FileText size={12} /> {attachment.name}
                <button type="button" onClick={() => onRemoveAttachment(index)} title="Remove">×</button>
              </span>
            ))}
          </div>
        ) : null}
        <div className="composerRow">
          <input
            ref={fileRef}
            type="file"
            hidden
            multiple
            onChange={(event) => {
              const files = Array.from(event.target.files ?? []);
              for (const file of files) onUpload(file);
              event.target.value = "";
            }}
          />
          <button type="button" className="iconButton" title="Upload image, text or PDF" onClick={() => fileRef.current?.click()}>
            <Paperclip size={17} />
          </button>
          <textarea
            value={text}
            placeholder={connecting ? "Connecting to the agent…" : busy ? "Agent is running…" : `Ask about ${project.name}…`}
            rows={2}
            onChange={(event) => setText(event.target.value)}
            onCompositionStart={() => { composing.current = true; }}
            onCompositionEnd={() => { composing.current = false; compositionEndedAt.current = Date.now(); }}
            onKeyDown={(event) => {
              if (composing.current || event.nativeEvent.isComposing || event.keyCode === 229 ||
                  Date.now() - compositionEndedAt.current < 60) return;
              if (event.key === "Enter" && !event.shiftKey) {
                event.preventDefault();
                submit();
              }
            }}
            disabled={blocked}
          />
          {busy ? (
            <button type="button" className="sendButton stop" onClick={onInterrupt} title="Stop">
              <CircleStop size={18} />
            </button>
          ) : (
            <button type="button" className="sendButton" onClick={submit} disabled={blocked || !text.trim()} title="Send (Enter)">
              <Send size={17} />
            </button>
          )}
        </div>
      </footer>
    </section>
  );
}

function ChatItemView({ item, onSend, onLoopDecision }: { item: ChatItem; onSend: (text: string) => void; onLoopDecision: (checkId: string, action: "continue" | "stop") => void }) {
  if (item.kind === "user") {
    return (
      <article className="chatMessage userMessage">
        <div className="chatAvatar"><User size={14} /></div>
        <div className="chatBubble">{item.text}</div>
      </article>
    );
  }
  if (item.kind === "assistant") {
    return (
      <article className="chatMessage assistantMessage">
        <div className="chatAvatar"><Bot size={14} /></div>
        <div className="chatBubble">
          {item.thinking ? <ThinkingBlock text={item.thinking} /> : null}
          {item.tools.length > 0 ? <ToolActivity tools={item.tools} /> : null}
          {item.text ? <Markdown text={item.text} /> : null}
        </div>
      </article>
    );
  }
  if (item.kind === "tool") {
    return (
      <article className="chatMessage assistantMessage">
        <div className="chatAvatar"><Bot size={14} /></div>
        <div className="chatBubble"><ToolActivity tools={[item.call]} /></div>
      </article>
    );
  }
  if (item.kind === "approval") {
    return (
      <article className="chatMessage assistantMessage">
        <div className="chatAvatar"><Bot size={14} /></div>
        <div className="chatBubble">
          <ApprovalView id={item.id} card={item.card} decision={item.decision} onDecision={() => undefined} active={false} />
        </div>
      </article>
    );
  }
  if (item.kind === "compaction") {
    return (
      <div className="chatMarker">
        <FileText size={13} /> Compacted {formatTokens(item.before)} → {formatTokens(item.after)}
        {item.hardTrimmed ? " (hard trimmed)" : ""}
      </div>
    );
  }
  if (item.kind === "loop") {
    return (
      <div className={`loopBubble${item.checkId ? " action" : ""}${item.decision ? " decided" : ""}`}>
        <div className="loopMessage"><Wrench size={13} /><span>{item.message}</span></div>
        {item.summary ? <div className="loopSummary">{item.summary}</div> : null}
        {item.checkId && !item.decision ? (
          <div className="loopActions">
            <button type="button" className="allow" onClick={() => onLoopDecision(item.checkId!, "continue")}>Continue</button>
            <button type="button" className="deny" onClick={() => onLoopDecision(item.checkId!, "stop")}>Stop</button>
          </div>
        ) : item.decision ? (
          <small>{item.decision === "continue" ? "Continued" : "Stopped"}</small>
        ) : null}
      </div>
    );
  }
  if (item.kind === "notice") {
    return (
      <div className="chatMarker noticeMarker">
        <span>{item.message}</span>
        {item.action ? (
          <button type="button" onClick={() => onSend(item.action!.prompt)}>
            {item.action.label}
          </button>
        ) : null}
      </div>
    );
  }
  return <div className="chatError">{item.message}</div>;
}

function LiveView({
  live,
  status,
  onDecision
}: {
  live: LiveTurn;
  status: ChatStatus;
  onDecision: (id: string, action: "allow" | "deny" | "always" | "edit", text?: string) => void;
}) {
  return (
    <article className="chatMessage assistantMessage liveMessage">
      <div className="chatAvatar"><Bot size={14} /></div>
      <div className="chatBubble">
        {live.thinking ? <ThinkingBlock text={live.thinking} streaming={status === "streaming" && !live.text} /> : null}
        {live.approval ? (
          <ApprovalView
            id={live.approval.id}
            card={live.approval.card}
            decision={live.approval.decision}
            onDecision={onDecision}
            active
          />
        ) : null}
        {live.tools.length > 0 ? <ToolActivity tools={live.tools} /> : null}
        {live.text ? <Markdown text={live.text} /> : null}
        {status === "streaming" && !live.text && !live.thinking && !live.approval && live.tools.length === 0 ? <span className="typingDots">…</span> : null}
      </div>
    </article>
  );
}

function ThinkingBlock({ text, streaming }: { text: string; streaming?: boolean }) {
  return (
    <details className="thinkingBlock" open={Boolean(streaming)}>
      <summary>
        <Sparkles size={13} /> {streaming ? "Thinking…" : "Thinking"}
      </summary>
      <pre>{text}</pre>
    </details>
  );
}

function ToolActivity({ tools }: { tools: ToolCall[] }) {
  if (tools.length === 0) return null;
  const names = Array.from(new Set(tools.map((tool) => tool.name))).join(", ");
  const running = tools.some((tool) => !tool.result);
  const failed = tools.some((tool) => tool.result?.is_error);
  return (
    <details className="toolActivity">
      <summary>
        <Wrench size={12} />
        <span>{tools.length} tool {tools.length === 1 ? "call" : "calls"}</span>
        <span className="toolActivityNames">{names}</span>
        <span className={`toolActivityState${failed ? " error" : running ? " running" : ""}`}>
          {failed ? "error" : running ? "running…" : "done"}
        </span>
      </summary>
      <div className="toolActivityBody">
        {tools.map((tool) => <ToolCard key={tool.id} call={tool} />)}
      </div>
    </details>
  );
}

function ToolCard({ call }: { call: ToolCall }) {
  return (
    <div className={`toolCard${call.result?.is_error ? " error" : ""}`}>
      <div className="toolCardHeader">
        <span className="toolName">{call.name}</span>
        <span className="toolSummary">{call.summary}</span>
        {call.result ? <span className="toolStatus">{call.result.is_error ? "error" : "done"}</span> : <span className="toolStatus running">running</span>}
      </div>
      {call.diff ? <DiffSummary diff={call.diff} /> : null}
      {call.result ? (
        <pre className="toolOutput">{clip(call.result.text, 4000)}</pre>
      ) : null}
    </div>
  );
}

function ApprovalView({
  id,
  card,
  decision,
  onDecision,
  active = false
}: {
  id: string;
  card: ApprovalCard;
  decision?: string;
  onDecision: (id: string, action: "allow" | "deny" | "always" | "edit", text?: string) => void;
  active?: boolean;
}) {
  const [editing, setEditing] = useState(false);
  const [draft, setDraft] = useState(card.editable ?? "");
  useEffect(() => setDraft(card.editable ?? ""), [card.editable]);
  useEffect(() => {
    if (!active || decision) return;
    const handler = (event: KeyboardEvent) => {
      if (event.defaultPrevented) return;
      const target = event.target as HTMLElement | null;
      if (target && (target.tagName === "INPUT" || target.tagName === "TEXTAREA" || target.isContentEditable)) {
        return;
      }
      if (event.key === "Escape") {
        event.preventDefault();
        onDecision(id, "deny");
      } else if (event.key === "Enter" && !event.shiftKey) {
        event.preventDefault();
        onDecision(id, "allow");
      }
    };
    window.addEventListener("keydown", handler);
    return () => window.removeEventListener("keydown", handler);
  }, [active, decision, id, onDecision]);
  if (decision) return null;
  return (
    <div className="approvalCard">
      <div className="approvalHeader">
        <strong>Approval needed</strong>
        <span>{card.tool}</span>
      </div>
      <div className="approvalSummary">{card.summary}</div>
      {card.detail ? <pre className="approvalDetail">{card.detail}</pre> : null}
      {card.outside_project ? <div className="approvalWarning">⚠ This path is outside the project.</div> : null}
      {card.danger ? <div className="approvalWarning">⚠ {card.danger}</div> : null}
      {card.diff ? <DiffSummary diff={card.diff} /> : null}
      {editing ? (
        <div className="approvalEdit">
          <textarea value={draft} onChange={(event) => setDraft(event.target.value)} rows={6} />
          <div>
            <button type="button" className="allow" onClick={() => { onDecision(id, "edit", draft); setEditing(false); }}>Apply</button>
            <button type="button" onClick={() => setEditing(false)}>Cancel</button>
          </div>
        </div>
      ) : (
        <div className="approvalActions">
          <button type="button" className="allow" onClick={() => onDecision(id, "allow")}>Allow</button>
          <button type="button" title="Allow this exact call for the rest of the session" onClick={() => onDecision(id, "always")}>Always</button>
          {card.editable ? <button type="button" onClick={() => { setDraft(card.editable ?? ""); setEditing(true); }}>Edit</button> : null}
          <button type="button" className="deny" onClick={() => onDecision(id, "deny")}>Deny</button>
        </div>
      )}
    </div>
  );
}

function DiffSummary({ diff }: { diff: ToolDiff }) {
  return (
    <details className="diffCard">
      <summary>
        <span className="diffPath">{diff.path}</span>
        <span className="diffStat add">+{diff.added}</span>
        <span className="diffStat remove">-{diff.removed}</span>
      </summary>
      <div className="diffBody">
        <pre className="diffBefore">{diff.before}</pre>
        <pre className="diffAfter">{diff.after}</pre>
      </div>
    </details>
  );
}

function Markdown({ text }: { text: string }) {
  return (
    <div className="markdown">
      <ReactMarkdown remarkPlugins={[remarkGfm]}>{text}</ReactMarkdown>
    </div>
  );
}

function statusLabel(status: ChatStatus) {
  if (status === "connecting") return "connecting";
  if (status === "streaming") return "streaming";
  if (status === "approval") return "waiting";
  if (status === "busy") return "busy";
  return "idle";
}

function formatTokens(value: number) {
  if (!value) return "–";
  if (value >= 1_000_000) return `${(value / 1_000_000).toFixed(1)}M`;
  if (value >= 1_000) return `${(value / 1_000).toFixed(1)}k`;
  return String(value);
}

function clip(text: string, limit: number) {
  if (text.length <= limit) return text;
  return `${text.slice(0, limit)}\n… ${text.length - limit} chars truncated`;
}
