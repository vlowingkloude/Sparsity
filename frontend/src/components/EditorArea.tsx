import { Editor, type BeforeMount, type OnMount } from "@monaco-editor/react";
import { AlertTriangle, FileCode2, ListChecks, RotateCcw, Save, Sparkles, X } from "lucide-react";
import { useEffect, useMemo, useRef, useState } from "react";
import { imageUrl } from "../api";
import {
  createLspClient,
  type LspDiagnostic,
  type LspStatus,
  supportsLsp,
  type WorkspaceLspClient
} from "../lspClient";
import type { OpenDocument, ProjectInfo } from "../types";
import { fileName, fileUri, languageForPath } from "../utils";

type RevealLocation = { path: string; line: number; column: number; token: number } | null;

type EditorAreaProps = {
  project: ProjectInfo | null;
  documents: OpenDocument[];
  activeDocument: OpenDocument | null;
  revealLocation: RevealLocation;
  onActivate: (path: string) => void;
  onClose: (path: string) => void;
  onChange: (content: string) => void;
  onSave: (contentOverride?: string) => Promise<void>;
  onReload: (path: string) => void;
  onOpenPath: (path: string, line: number, column: number) => void;
  onQuickOpen: () => void;
};

export function EditorArea({
  project,
  documents,
  activeDocument,
  revealLocation,
  onActivate,
  onClose,
  onChange,
  onSave,
  onReload,
  onOpenPath,
  onQuickOpen
}: EditorAreaProps) {
  const editorRef = useRef<Parameters<OnMount>[0] | null>(null);
  const monacoRef = useRef<Parameters<OnMount>[1] | null>(null);
  const clientsRef = useRef<Map<string, WorkspaceLspClient>>(new Map());
  const openedRef = useRef<Map<string, Set<string>>>(new Map());
  const [mountVersion, setMountVersion] = useState(0);
  const [lspStatuses, setLspStatuses] = useState<Record<string, LspStatus>>({});
  const [diagnostics, setDiagnostics] = useState<Record<string, LspDiagnostic[]>>({});
  const [problemsOpen, setProblemsOpen] = useState(false);
  const [formatOnSave, setFormatOnSave] = useState(() => window.localStorage.getItem("sparsity.formatOnSave") === "true");
  const [autoSave, setAutoSave] = useState(() => window.localStorage.getItem("sparsity.autoSave") === "true");
  const [wordWrap, setWordWrap] = useState(() => window.localStorage.getItem("sparsity.wordWrap") !== "false");
  const language = activeDocument ? languageForPath(activeDocument.path) : "plaintext";
  const allDiagnostics = useMemo(() => Object.values(diagnostics).flat(), [diagnostics]);
  const lspStatus = lspStatuses[language] ?? (supportsLsp(language) ? "connecting" : "closed");

  useEffect(() => {
    for (const client of clientsRef.current.values()) client.dispose();
    clientsRef.current.clear();
    openedRef.current.clear();
    setLspStatuses({});
    setDiagnostics({});
    return () => {
      for (const client of clientsRef.current.values()) client.dispose();
      clientsRef.current.clear();
      openedRef.current.clear();
    };
  }, [project?.id, project?.root, project?.initial_file]);

  useEffect(() => {
    const editor = editorRef.current;
    const monacoApi = monacoRef.current;
    if (!project || !activeDocument || activeDocument.kind !== "text" || activeDocument.loading || activeDocument.error || !supportsLsp(language) || !editor || !monacoApi) return;

    let client = clientsRef.current.get(language);
    if (!client) {
      client = createLspClient({
        monacoApi,
        workspace: project,
        projectId: project.id,
        language,
        onStatusChange: (status) => setLspStatuses((current) => ({ ...current, [language]: status })),
        onDiagnostics: (path, next) => setDiagnostics((current) => ({ ...current, [path]: next }))
      });
      clientsRef.current.set(language, client);
      openedRef.current.set(language, new Set());
      client.start();
    }
    const model = editor.getModel();
    if (!model || model.uri.toString() !== fileUri(project.root, activeDocument.path)) return;
    client.openDocument(activeDocument.path, model);
    openedRef.current.get(language)?.add(activeDocument.path);
  }, [activeDocument?.path, activeDocument?.kind, activeDocument?.loading, activeDocument?.error, language, mountVersion, project?.id, project?.root, project?.initial_file]);

  useEffect(() => {
    const paths = new Set(documents.map((document) => document.path));
    for (const [clientLanguage, opened] of openedRef.current) {
      const client = clientsRef.current.get(clientLanguage);
      if (!client) continue;
      for (const path of [...opened]) {
        if (!paths.has(path)) {
          client.closeDocument(path);
          opened.delete(path);
        }
      }
    }
  }, [documents]);

  useEffect(() => {
    if (!revealLocation || revealLocation.path !== activeDocument?.path) return;
    window.requestAnimationFrame(() => {
      const editor = editorRef.current;
      if (!editor) return;
      const position = { lineNumber: revealLocation.line, column: revealLocation.column };
      editor.setPosition(position);
      editor.revealPositionInCenter(position);
      editor.focus();
    });
  }, [activeDocument?.path, mountVersion, revealLocation?.token]);

  useEffect(() => {
    const handleSave = (event: KeyboardEvent) => {
      if ((event.ctrlKey || event.metaKey) && event.key.toLowerCase() === "s") {
        event.preventDefault();
        void saveActive();
      }
      if ((event.ctrlKey || event.metaKey) && event.shiftKey && event.key.toLowerCase() === "o" && editorRef.current) {
        event.preventDefault();
        void editorRef.current.getAction("editor.action.quickOutline")?.run();
      }
    };
    window.addEventListener("keydown", handleSave);
    return () => window.removeEventListener("keydown", handleSave);
  });

  useEffect(() => {
    if (!autoSave || !activeDocument || activeDocument.kind !== "text" || activeDocument.loading || activeDocument.error || activeDocument.externalChanged || activeDocument.content === activeDocument.savedContent) return;
    const timer = window.setTimeout(() => void saveActive(), 1000);
    return () => window.clearTimeout(timer);
  }, [
    activeDocument?.content,
    activeDocument?.error,
    activeDocument?.externalChanged,
    activeDocument?.loading,
    activeDocument?.path,
    activeDocument?.savedContent,
    autoSave,
    formatOnSave,
    lspStatus
  ]);

  const beforeMount: BeforeMount = (monacoApi) => {
    monacoApi.editor.defineTheme("sparsity-jetbrains-light", {
      base: "vs",
      inherit: true,
      rules: [
        { token: "comment", foreground: "8c8c8c", fontStyle: "italic" },
        { token: "string", foreground: "067d17" },
        { token: "number", foreground: "2f5fbf" },
        { token: "keyword", foreground: "0033b3" },
        { token: "type", foreground: "00627a" },
        { token: "type.identifier", foreground: "00627a" },
        { token: "identifier", foreground: "1f2328" },
        { token: "delimiter", foreground: "1f2328" },
        { token: "operator", foreground: "1f2328" },
        { token: "keyword.directive", foreground: "0033b3" }
      ],
      colors: {
        "editor.background": "#ffffff", "editor.foreground": "#1f2328", "editorGutter.background": "#ffffff",
        "editorLineNumber.foreground": "#a8a8a8", "editorLineNumber.activeForeground": "#1f2328",
        "editorCursor.foreground": "#000000", "editor.selectionBackground": "#e8f0fe",
        "editor.inactiveSelectionBackground": "#e9ecef", "editor.lineHighlightBackground": "#f5f7fa",
        "editorWhitespace.foreground": "#e3e5e8", "editorIndentGuide.background1": "#e3e5e8",
        "editorIndentGuide.activeBackground1": "#c9ccd6", "editorWidget.background": "#f7f8fa",
        "editorSuggestWidget.background": "#ffffff", "editorSuggestWidget.selectedBackground": "#e8f0fe"
      }
    });

    if (!monacoApi.languages.getLanguages().some((language) => language.id === "cpp")) {
      monacoApi.languages.register({
        id: "cpp",
        extensions: [".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx", ".cppm", ".ixx", ".mpp", ".ccm", ".cxxm"]
      });
    }
    monacoApi.languages.setMonarchTokensProvider("cpp", {
      keywords: [
        "alignas", "alignof", "and", "and_eq", "asm", "atomic_cancel", "atomic_commit", "atomic_noexcept",
        "bitand", "bitor", "break", "case", "catch", "class", "compl", "concept", "const", "consteval",
        "constexpr", "constinit", "const_cast", "continue", "co_await", "co_return", "co_yield", "decltype",
        "default", "delete", "do", "dynamic_cast", "else", "enum", "explicit", "export", "extern", "false",
        "final", "for", "friend", "goto", "if", "import", "inline", "module", "mutable", "namespace", "new",
        "noexcept", "not", "not_eq", "nullptr", "operator", "or", "or_eq", "override", "private", "protected",
        "public", "reflexpr", "register", "reinterpret_cast", "requires", "return", "sizeof", "static",
        "static_assert", "static_cast", "struct", "switch", "synchronized", "template", "this", "thread_local",
        "throw", "true", "try", "typedef", "typeid", "typename", "union", "using", "virtual", "volatile",
        "wchar_t", "while", "xor", "xor_eq"
      ],
      typeKeywords: [
        "auto", "bool", "char", "char8_t", "char16_t", "char32_t", "double", "float", "int", "long", "short",
        "signed", "unsigned", "void"
      ],
      std: [
        "std", "size_t", "string", "string_view", "vector", "array", "span", "optional", "expected", "unique_ptr",
        "shared_ptr", "weak_ptr", "map", "unordered_map", "set", "unordered_set", "tuple", "pair", "function",
        "filesystem", "chrono", "mutex", "thread", "atomic", "format", "print", "ranges", "views"
      ],
      tokenizer: {
        root: [
          [/^[ \t]*#[ \t]*[a-zA-Z_]\w*/, "keyword.directive"],
          [/\/\/.*$/, "comment"],
          [/\/\*/, "comment", "@comment"],
          [/[a-zA-Z_]\w*/, { cases: { "@keywords": "keyword", "@typeKeywords": "type", "@std": "type.identifier", "@default": "identifier" } }],
          [/(0[xX][0-9a-fA-F]+|0[bB][01]+|\d+)[uUlL]*/, "number"],
          [/\d+\.\d+([eE][-+]?\d+)?[fFlL]?/, "number"],
          [/"([^"\\]|\\.)*$/, "string.invalid"],
          [/"/, "string", "@string"],
          [/'([^'\\]|\\.)*'/, "string"],
          [/[{}()\[\]]/, "delimiter"],
          [/[<>]/, "delimiter"],
          [/[~!%^&*+=|?:<>\/-]/, "operator"],
          [/[;,.\[\]]/, "delimiter"]
        ],
        comment: [
          [/[^/*]+/, "comment"],
          [/\*\//, "comment", "@pop"],
          [/[/*]/, "comment"]
        ],
        string: [
          [/[^\\"]+/, "string"],
          [/\\./, "string.escape"],
          [/"/, "string", "@pop"]
        ]
      }
    });
    monacoApi.languages.setLanguageConfiguration("cpp", {
      comments: { lineComment: "//", blockComment: ["/*", "*/"] },
      brackets: [["{", "}"], ["[", "]"], ["(", ")"]],
      autoClosingPairs: [
        { open: "{", close: "}" }, { open: "[", close: "]" }, { open: "(", close: ")" },
        { open: "\"", close: "\"", notIn: ["string", "comment"] },
        { open: "'", close: "'", notIn: ["string", "comment"] }
      ],
      surroundingPairs: [
        { open: "{", close: "}" }, { open: "[", close: "]" }, { open: "(", close: ")" },
        { open: "\"", close: "\"" }, { open: "'", close: "'" }
      ]
    });
  };

  const handleMount: OnMount = (editor, monacoApi) => {
    editorRef.current = editor;
    monacoRef.current = monacoApi;
    setMountVersion((version) => version + 1);
  };

  function formatActive() {
    if (!activeDocument) return;
    void clientsRef.current.get(language)?.format(activeDocument.path);
  }

  async function saveActive() {
    if (!activeDocument) return;
    const client = clientsRef.current.get(language);
    if (formatOnSave && client && lspStatus === "ready") await client.format(activeDocument.path);
    const currentContent = editorRef.current?.getModel()?.uri.toString() === fileUri(project!.root, activeDocument.path)
      ? editorRef.current.getValue()
      : activeDocument.content;
    await onSave(currentContent);
    client?.didSave(activeDocument.path);
  }

  if (!project) {
    return (
      <section className="editorArea welcomeArea">
        <div className="welcomeMark">S</div>
        <h1>Sparsity</h1>
        <p>Select a project from the sidebar to start editing.</p>
      </section>
    );
  }

  return (
    <section className="editorArea">
      <div className="editorTabs">
        {documents.map((document) => {
          const dirty = document.content !== document.savedContent;
          return (
            <button type="button" key={document.path} className={`editorTab${activeDocument?.path === document.path ? " active" : ""}`} onClick={() => onActivate(document.path)} title={document.path}>
              <span className={dirty ? "dirtyDot" : "fileDot"}>{dirty ? "●" : document.externalChanged ? "!" : ""}</span>
              <span>{fileName(document.path)}</span>
              <span className="closeTab" role="button" tabIndex={0} onClick={(event) => { event.stopPropagation(); onClose(document.path); }} onKeyDown={(event) => {
                if (event.key === "Enter" || event.key === " ") { event.stopPropagation(); onClose(document.path); }
              }}><X size={13} /></span>
            </button>
          );
        })}
      </div>

      {activeDocument ? (
        <>
          <div className="editorToolbar">
            <div className="breadcrumbs">{activeDocument.path.split("/").map((part, index) => <span key={`${part}-${index}`}>{part}</span>)}</div>
            <div className="editorMeta">
              {supportsLsp(language) && activeDocument.kind === "text" ? <span className={`lspStatus ${lspStatus}`}>{language}: {lspStatus}</span> : null}
              {allDiagnostics.length ? <button type="button" onClick={() => setProblemsOpen((value) => !value)}><ListChecks size={14} /> {allDiagnostics.length} Problems</button> : null}
              <label className="saveOption" title="Wrap long lines at the editor width"><input type="checkbox" checked={wordWrap} onChange={(event) => { setWordWrap(event.target.checked); window.localStorage.setItem("sparsity.wordWrap", String(event.target.checked)); }} /> Wrap</label>
              <label className="saveOption" title="Automatically save one second after typing"><input type="checkbox" checked={autoSave} onChange={(event) => { setAutoSave(event.target.checked); window.localStorage.setItem("sparsity.autoSave", String(event.target.checked)); }} /> Auto</label>
              <label className="saveOption" title="Format with the language server before saving"><input type="checkbox" checked={formatOnSave} onChange={(event) => { setFormatOnSave(event.target.checked); window.localStorage.setItem("sparsity.formatOnSave", String(event.target.checked)); }} /> Format on save</label>
              {supportsLsp(language) && activeDocument.kind === "text" ? <button type="button" onClick={formatActive} disabled={lspStatus !== "ready"} title="Format document (Shift+Alt+F)"><Sparkles size={14} /> Format</button> : null}
              {activeDocument.kind === "text" ? <button type="button" onClick={saveActive} disabled={activeDocument.content === activeDocument.savedContent && !activeDocument.externalChanged || activeDocument.loading} title="Save (Ctrl+S)"><Save size={15} /> Save</button> : null}
            </div>
          </div>
          <div className="editorSurface">
            {activeDocument.externalChanged ? (
              <div className="externalChangeBanner">
                <AlertTriangle size={14} /> The file changed on disk.
                <button type="button" onClick={() => onReload(activeDocument.path)}><RotateCcw size={13} /> Reload</button>
                <span>Save to overwrite the disk version.</span>
              </div>
            ) : null}
            {activeDocument.loading ? <div className="editorMessage">Opening {activeDocument.path}…</div> : null}
            {activeDocument.error ? <div className="editorMessage errorMessage">{activeDocument.error}</div> : null}
            {!activeDocument.loading && !activeDocument.error && activeDocument.kind === "image" ? (
              <div className="imagePreview"><img src={`${imageUrl(project.id, activeDocument.path)}&v=${activeDocument.revision}`} alt={activeDocument.path} /><span>{activeDocument.path}</span></div>
            ) : null}
            {!activeDocument.loading && !activeDocument.error && activeDocument.kind === "text" ? (
              <Editor
                height="100%"
                beforeMount={beforeMount}
                onMount={handleMount}
                language={language}
                path={fileUri(project.root, activeDocument.path)}
                value={activeDocument.content}
                onChange={(value) => onChange(value ?? "")}
                theme="sparsity-jetbrains-light"
                saveViewState
                keepCurrentModel
                options={{
                  automaticLayout: true, fontFamily: "'JetBrains Mono', 'SFMono-Regular', Consolas, monospace",
                  fontSize: 14, lineHeight: 22, minimap: { enabled: true, scale: 1 }, padding: { top: activeDocument.externalChanged ? 43 : 10 },
                  scrollBeyondLastLine: false, smoothScrolling: true, bracketPairColorization: { enabled: true },
                  guides: { bracketPairs: true, indentation: true }, renderWhitespace: "selection", wordWrap: wordWrap ? "on" : "off",
                  quickSuggestions: { other: true, comments: false, strings: false }, suggestOnTriggerCharacters: true,
                  acceptSuggestionOnEnter: "on", tabCompletion: "on"
                }}
              />
            ) : null}
            {problemsOpen ? (
              <section className="problemsPanel">
                <header><strong>Problems ({allDiagnostics.length})</strong><button type="button" onClick={() => setProblemsOpen(false)}><X size={14} /></button></header>
                <div>{allDiagnostics.map((diagnostic, index) => (
                  <button type="button" key={`${diagnostic.path}-${index}`} onClick={() => onOpenPath(diagnostic.path, diagnostic.range.start.line + 1, diagnostic.range.start.character + 1)}>
                    <AlertTriangle size={14} /><span><strong>{diagnostic.message}</strong><small>{diagnostic.path}:{diagnostic.range.start.line + 1}:{diagnostic.range.start.character + 1}</small></span>
                  </button>
                ))}</div>
              </section>
            ) : null}
          </div>
        </>
      ) : (
        <div className="noEditor">
          <FileCode2 size={38} />
          <p>Select a file from the Explorer to begin.</p>
          <button type="button" className="quickOpenLink" onClick={onQuickOpen}>Quick Open <kbd>Ctrl P</kbd></button>
        </div>
      )}
    </section>
  );
}
