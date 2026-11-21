import { FitAddon } from "@xterm/addon-fit";
import { Terminal } from "@xterm/xterm";
import { Maximize2, Minimize2, Minus, Plus, RotateCw, SquareTerminal, X } from "lucide-react";
import { useEffect, useRef, useState } from "react";
import { terminalUrl } from "../api";
import "@xterm/xterm/css/xterm.css";

type ServerMessage =
  | { type: "output"; data: string }
  | { type: "error"; message: string }
  | { type: "exit"; code: number | null };
type Status = "connecting" | "connected" | "closed" | "exited";
type SessionInfo = { id: number; generation: number; status: Status };

export function TerminalPanel({ visible, onMinimize, cwd }: { visible: boolean; onMinimize: () => void; cwd?: string }) {
  const nextId = useRef(2);
  const [expanded, setExpanded] = useState(false);
  const [sessions, setSessions] = useState<SessionInfo[]>([{ id: 1, generation: 0, status: "connecting" }]);
  const [activeId, setActiveId] = useState(1);

  function addSession() {
    const id = nextId.current++;
    setSessions((current) => [...current, { id, generation: 0, status: "connecting" }]);
    setActiveId(id);
  }

  function closeSession(id: number) {
    if (sessions.length === 1) {
      const replacement = { id: nextId.current++, generation: 0, status: "connecting" as const };
      setSessions([replacement]);
      setActiveId(replacement.id);
      return;
    }
    const index = sessions.findIndex((session) => session.id === id);
    const remaining = sessions.filter((session) => session.id !== id);
    setSessions(remaining);
    if (activeId === id) setActiveId(remaining[Math.min(index, remaining.length - 1)].id);
  }

  return (
    <section className={`terminalPanel${expanded ? " expanded" : ""}${visible ? "" : " hidden"}`}>
      <header>
        <div className="terminalTabs">
          <SquareTerminal size={15} />
          {sessions.map((session, index) => (
            <button type="button" key={session.id} className={`terminalTab${activeId === session.id ? " active" : ""}`} onClick={() => setActiveId(session.id)}>
              <span>{index + 1}</span><span className={`terminalStatus ${session.status}`}>●</span>
              <span role="button" className="terminalTabClose" onClick={(event) => { event.stopPropagation(); closeSession(session.id); }}><X size={11} /></span>
            </button>
          ))}
          <button type="button" onClick={addSession} title="New terminal"><Plus size={14} /></button>
        </div>
        <div>
          <button type="button" onClick={() => setSessions((current) => current.map((session) => session.id === activeId ? { ...session, generation: session.generation + 1, status: "connecting" } : session))} title="Restart terminal"><RotateCw size={15} /></button>
          <button type="button" onClick={() => setExpanded((value) => !value)} title="Resize terminal">{expanded ? <Minimize2 size={15} /> : <Maximize2 size={15} />}</button>
          <button type="button" onClick={onMinimize} title="Minimize terminal"><Minus size={16} /></button>
        </div>
      </header>
      <div className="terminalSessions">
        {sessions.map((session) => (
          <TerminalSession
            key={session.id}
            generation={session.generation}
            cwd={cwd}
            visible={visible && activeId === session.id}
            onStatus={(status) => setSessions((current) => current.map((candidate) => candidate.id === session.id ? { ...candidate, status } : candidate))}
          />
        ))}
      </div>
    </section>
  );
}

function TerminalSession({ generation, visible, cwd, onStatus }: { generation: number; visible: boolean; cwd?: string; onStatus: (status: Status) => void }) {
  const surfaceRef = useRef<HTMLDivElement | null>(null);
  const fitRef = useRef<FitAddon | null>(null);
  const visibleRef = useRef(visible);
  visibleRef.current = visible;

  useEffect(() => {
    if (!surfaceRef.current) return;
    const surface = surfaceRef.current;
    const terminal = new Terminal({
      cursorBlink: true, convertEol: true, fontFamily: "'JetBrains Mono', 'SFMono-Regular', Consolas, monospace", fontSize: 13, lineHeight: 1.25,
      theme: {
        background: "#ffffff", foreground: "#1f2328", cursor: "#000000", cursorAccent: "#ffffff", selectionBackground: "#d4e2ff",
        black: "#000000", red: "#db3b4b", green: "#1e8c3a", yellow: "#b26b00", blue: "#5b7db1", magenta: "#8a6fa8", cyan: "#00627a", white: "#f2f3f5",
        brightBlack: "#6c707e", brightRed: "#e5534b", brightGreen: "#2ea043", brightYellow: "#d29922", brightBlue: "#4b8bff", brightMagenta: "#a371f7", brightCyan: "#2b9ab3", brightWhite: "#ffffff"
      }
    });
    const fit = new FitAddon();
    terminal.loadAddon(fit);
    terminal.open(surface);
    fitRef.current = fit;
    const socket = new WebSocket(terminalUrl(cwd));
    let exited = false;
    onStatus("connecting");
    const fitAndResize = () => {
      if (!visibleRef.current) return;
      try {
        fit.fit();
        if (socket.readyState === WebSocket.OPEN) socket.send(JSON.stringify({ type: "resize", cols: terminal.cols, rows: terminal.rows }));
      } catch { /* Hidden or resizing surfaces can briefly have no measurable size. */ }
    };
    const observer = new ResizeObserver(fitAndResize);
    observer.observe(surface);
    socket.addEventListener("open", () => { onStatus("connected"); fitAndResize(); if (visibleRef.current) terminal.focus(); });
    socket.addEventListener("message", (event) => {
      const message = parseMessage(event.data);
      if (!message) return;
      if (message.type === "output") terminal.write(message.data);
      if (message.type === "error") terminal.writeln(`\r\n\x1b[31m${message.message}\x1b[0m`);
      if (message.type === "exit") { exited = true; onStatus("exited"); terminal.writeln(`\r\n\x1b[90mProcess exited${message.code == null ? "" : ` with ${message.code}`}.\x1b[0m`); }
    });
    socket.addEventListener("close", () => { if (!exited) onStatus("closed"); });
    socket.addEventListener("error", () => { onStatus("closed"); terminal.writeln("\r\n\x1b[31mTerminal connection failed.\x1b[0m"); });
    const input = terminal.onData((data) => { if (socket.readyState === WebSocket.OPEN) socket.send(JSON.stringify({ type: "input", data })); });
    requestAnimationFrame(fitAndResize);
    return () => { input.dispose(); observer.disconnect(); socket.close(); terminal.dispose(); fitRef.current = null; };
  }, [generation, cwd]);

  useEffect(() => {
    if (visible) requestAnimationFrame(() => fitRef.current?.fit());
  }, [visible]);

  return <div ref={surfaceRef} className={`terminalSurface${visible ? " active" : ""}`} />;
}

function parseMessage(value: unknown): ServerMessage | null {
  if (typeof value !== "string") return null;
  try { return JSON.parse(value) as ServerMessage; } catch { return null; }
}
