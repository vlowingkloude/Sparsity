import { lazy, Suspense, useEffect, useState } from "react";

const TerminalPanel = lazy(() => import("./TerminalPanel").then((module) => ({ default: module.TerminalPanel })));

export function TerminalLauncher({ visible, onMinimize }: { visible: boolean; onMinimize: () => void }) {
  const [started, setStarted] = useState(false);

  useEffect(() => {
    if (visible) setStarted(true);
  }, [visible]);

  if (!started) return null;
  return (
    <Suspense fallback={null}>
      <TerminalPanel visible={visible} onMinimize={onMinimize} />
    </Suspense>
  );
}
