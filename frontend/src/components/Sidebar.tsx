import { useEffect, useMemo, useState } from "react";
import {
  ChevronDown,
  ChevronRight,
  File,
  FileCode2,
  FileImage,
  FilePlus2,
  Folder,
  FolderGit2,
  FolderOpen,
  FolderPlus,
  RefreshCw,
  Search,
  X
} from "lucide-react";
import type { PlaygroundInfo, ProjectInfo, TreeEntry } from "../types";
import { isImagePath, parentPaths } from "../utils";

type SidebarProps = {
  playground: PlaygroundInfo | null;
  entries: TreeEntry[];
  loading: boolean;
  loadingDirectories: Set<string>;
  projects: ProjectInfo[];
  activeProjectId: string | null;
  activePath: string | null;
  selectedPath: string | null;
  runningProjectIds: Set<string>;
  dirtyProjectIds: Set<string>;
  onSelectPath: (path: string) => void;
  onOpenPath: (path: string) => void;
  onExpand: (path: string) => void;
  onRefresh: () => void;
  onOpenProject: (path: string) => void;
  onActivateProject: (id: string) => void;
  onCloseProject: (id: string) => void;
  onCreate: (kind: "file" | "directory", parent: string) => void;
  onRename: (path: string) => void;
  onDelete: (path: string) => void;
  onSearch: () => void;
};

export function Sidebar({
  playground,
  entries,
  loading,
  loadingDirectories,
  projects,
  activeProjectId,
  activePath,
  selectedPath,
  runningProjectIds,
  dirtyProjectIds,
  onSelectPath,
  onOpenPath,
  onExpand,
  onRefresh,
  onOpenProject,
  onActivateProject,
  onCloseProject,
  onCreate,
  onRename,
  onDelete,
  onSearch
}: SidebarProps) {
  const [expanded, setExpanded] = useState<Set<string>>(new Set());
  const [menu, setMenu] = useState<{ path: string; kind: TreeEntry["kind"]; x: number; y: number } | null>(null);

  useEffect(() => setExpanded(new Set()), [playground?.root]);
  useEffect(() => {
    if (!menu) return;
    const close = () => setMenu(null);
    window.addEventListener("pointerdown", close);
    window.addEventListener("blur", close);
    return () => {
      window.removeEventListener("pointerdown", close);
      window.removeEventListener("blur", close);
    };
  }, [menu]);

  const visibleEntries = useMemo(
    () => entries.filter((entry) => parentPaths(entry.path).every((parent) => expanded.has(parent))),
    [expanded, entries]
  );

  function toggleDirectory(path: string) {
    setExpanded((current) => {
      const next = new Set(current);
      if (next.has(path)) {
        next.delete(path);
      } else {
        next.add(path);
        onExpand(path);
      }
      return next;
    });
  }

  return (
    <aside className="sidebar">
      <header className="sidebarHeader">
        <span>PLAYGROUND</span>
        <div className="sidebarActions">
          <button type="button" onClick={() => onCreate("file", "")} title="New file"><FilePlus2 size={15} /></button>
          <button type="button" onClick={() => onCreate("directory", "")} title="New folder"><FolderPlus size={15} /></button>
          <button type="button" onClick={onSearch} title="Search playground (Ctrl+Shift+F)"><Search size={15} /></button>
          <button type="button" onClick={onRefresh} title="Refresh"><RefreshCw size={14} /></button>
          <button
            type="button"
            onClick={() => onOpenProject(selectedPath ?? "")}
            title={selectedPath ? `Open ${selectedPath} as Project` : "Select a file or folder first"}
            disabled={!selectedPath}
          >
            <FolderGit2 size={15} />
          </button>
        </div>
      </header>

      {playground ? (
        <div className="workspaceTitle" title={playground.root}>
          <FolderOpen size={15} />
          <strong>{playground.name}</strong>
        </div>
      ) : null}

      <div className="treeScroll">
        {loading ? <div className="sidebarMessage"><RefreshCw className="spin" size={15} /> Loading…</div> : null}
        {!loading && visibleEntries.length === 0 ? <div className="sidebarMessage">This folder is empty.</div> : null}
        {visibleEntries.map((entry) => {
          const isDirectory = entry.kind === "directory";
          const isExpanded = expanded.has(entry.path);
          const isLoading = loadingDirectories.has(entry.path);
          return (
            <button
              type="button"
              key={entry.path}
              className={`treeRow${selectedPath === entry.path ? " selected" : ""}${activePath === entry.path ? " active" : ""}`}
              style={{ paddingLeft: `${8 + entry.depth * 15}px` }}
              title={entry.path}
              onClick={() => onSelectPath(entry.path)}
              onDoubleClick={() => isDirectory ? toggleDirectory(entry.path) : onOpenPath(entry.path)}
              onContextMenu={(event) => {
                event.preventDefault();
                onSelectPath(entry.path);
                setMenu({ path: entry.path, kind: entry.kind, x: event.clientX, y: event.clientY });
              }}
            >
              <span
                className="treeChevron"
                role="button"
                tabIndex={-1}
                onClick={(event) => {
                  if (!isDirectory) return;
                  event.stopPropagation();
                  toggleDirectory(entry.path);
                }}
              >
                {isDirectory ? isLoading ? <RefreshCw className="spin" size={13} />
                  : isExpanded ? <ChevronDown size={14} /> : <ChevronRight size={14} /> : null}
              </span>
              {isDirectory ? <Folder size={15} /> : fileIcon(entry.path)}
              <span>{entry.name}</span>
            </button>
          );
        })}
      </div>

      <section className="projectsPane">
        <header className="projectsHeader">
          <span>PROJECTS</span>
          <button
            type="button"
            className="openProjectButton"
            onClick={() => onOpenProject(selectedPath ?? "")}
            disabled={!selectedPath}
            title={selectedPath ? `Open ${selectedPath} as Project` : "Select a file or folder first"}
          >
            Open as Project
          </button>
        </header>
        <div className="projectList">
          {projects.length === 0 ? (
            <div className="sidebarMessage">
              Select a file or folder above, then click <strong>Open as Project</strong>.
            </div>
          ) : null}
          {projects.map((project) => {
            const active = project.id === activeProjectId;
            const running = runningProjectIds.has(project.id);
            const dirty = dirtyProjectIds.has(project.id);
            return (
              <div key={project.id} className={`projectRow${active ? " active" : ""}`}>
                <button type="button" className="projectMain" onClick={() => onActivateProject(project.id)} title={project.path}>
                  {project.kind === "file" ? <FileCode2 size={15} /> : <FolderOpen size={15} />}
                  <span className="projectText">
                    <strong>{project.name}</strong>
                    <small>{project.path}</small>
                  </span>
                  <span className="projectBadges">
                    {dirty ? <i className="dirtyDot" title="Unsaved changes">●</i> : null}
                    {running ? <span title="Agent running"><RefreshCw className="spin" size={12} /></span> : null}
                  </span>
                </button>
                <button type="button" className="projectClose" onClick={() => onCloseProject(project.id)} title="Close project">
                  <X size={13} />
                </button>
              </div>
            );
          })}
        </div>
      </section>

      {menu ? (
        <div className="treeContextMenu" style={{ left: menu.x, top: menu.y }} onPointerDown={(event) => event.stopPropagation()}>
          {menu.kind === "directory" ? <button type="button" onClick={() => { onCreate("file", menu.path); setMenu(null); }}>New file inside</button> : null}
          {menu.kind === "directory" ? <button type="button" onClick={() => { onCreate("directory", menu.path); setMenu(null); }}>New folder inside</button> : null}
          <button type="button" onClick={() => { onOpenProject(menu.path); setMenu(null); }}>Open as Project</button>
          <button type="button" onClick={() => { void navigator.clipboard?.writeText(menu.path); setMenu(null); }}>Copy relative path</button>
          <button type="button" onClick={() => { onRename(menu.path); setMenu(null); }}>Rename…</button>
          <button type="button" className="danger" onClick={() => { onDelete(menu.path); setMenu(null); }}>Delete…</button>
        </div>
      ) : null}
    </aside>
  );
}

function fileIcon(path: string) {
  if (isImagePath(path)) return <FileImage className="imageFileIcon" size={15} />;
  if (/\.(rs|ts|tsx|js|jsx|py|go|c|cpp|h|java)$/i.test(path)) return <FileCode2 className="codeFileIcon" size={15} />;
  return <File size={15} />;
}
