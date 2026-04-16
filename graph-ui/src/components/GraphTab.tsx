import { useEffect, useState, useCallback, useMemo } from "react";
import { Button } from "@/components/ui/button";
import {
  useGraphData,
  type ClusterMode,
  type ColorMode,
  type GraphMode,
} from "../hooks/useGraphData";
import {
  GraphScene,
  computeCameraTarget,
  computeFitAllTarget,
  type CameraTarget,
} from "./GraphScene";
import { Sidebar } from "./Sidebar";
import { FilterPanel } from "./FilterPanel";
import { NodeDetailPanel } from "./NodeDetailPanel";
import { ResizeHandle } from "./ResizeHandle";
import { ErrorBoundary } from "./ErrorBoundary";
import type { GraphNode, GraphData } from "../lib/types";

/* Persist panel widths */
function loadWidth(key: string, fallback: number): number {
  try {
    const v = localStorage.getItem(key);
    if (v) return Math.max(150, Math.min(600, parseInt(v, 10)));
  } catch { /* ignore */ }
  return fallback;
}
function saveWidth(key: string, value: number) {
  try { localStorage.setItem(key, String(Math.round(value))); } catch { /* ignore */ }
}

/* Persist boolean UI flags */
function loadBool(key: string, fallback: boolean): boolean {
  try {
    const v = localStorage.getItem(key);
    if (v === null) return fallback;
    return v === "true";
  } catch { return fallback; }
}
function saveBool(key: string, value: boolean) {
  try { localStorage.setItem(key, String(value)); } catch { /* ignore */ }
}

interface GraphTabProps {
  project: string | null;
}

export function GraphTab({ project }: GraphTabProps) {
  const { data, loading, error, fetchOverview } = useGraphData();
  const [highlightedIds, setHighlightedIds] = useState<Set<number> | null>(null);
  const [selectedPath, setSelectedPath] = useState<string | null>(null);
  const [selectedNode, setSelectedNode] = useState<GraphNode | null>(null);
  const [cameraTarget, setCameraTarget] = useState<CameraTarget | null>(null);
  const [showLabels, setShowLabels] = useState(true);
  const [clusterMode, setClusterMode] = useState<ClusterMode>("louvain");
  const [colorMode, setColorMode] = useState<ColorMode>("stellar");
  const [optimize, setOptimize] = useState<boolean>(false);
  const [graphMode, setGraphMode] = useState<GraphMode>("raw");
  const [isolateByEdge, setIsolateByEdge] = useState<boolean>(false);
  const [leftWidth, setLeftWidth] = useState(() => loadWidth("cbm-left-w", 260));
  const [rightWidth, setRightWidth] = useState(() => loadWidth("cbm-right-w", 280));
  const [showToggles, setShowToggles] = useState(() => loadBool("cbm-show-toggles", true));

  /* Filter state — all enabled by default */
  const [enabledLabels, setEnabledLabels] = useState<Set<string>>(new Set());
  const [enabledEdgeTypes, setEnabledEdgeTypes] = useState<Set<string>>(new Set());

  /* Initialize filters when data loads */
  useEffect(() => {
    if (!data) return;
    const labels = new Set(data.nodes.map((n) => n.label));
    const types = new Set(data.edges.map((e) => e.type));
    setEnabledLabels(labels);
    setEnabledEdgeTypes(types);
  }, [data]);

  /* Compute filtered data */
  const filteredData: GraphData | null = useMemo(() => {
    if (!data) return null;

    let nodes = data.nodes.filter((n) => enabledLabels.has(n.label));
    const nodeIds = new Set(nodes.map((n) => n.id));
    const edges = data.edges.filter(
      (e) =>
        enabledEdgeTypes.has(e.type) &&
        nodeIds.has(e.source) &&
        nodeIds.has(e.target),
    );

    if (isolateByEdge) {
      const connectedIds = new Set<number>();
      for (const e of edges) {
        connectedIds.add(e.source);
        connectedIds.add(e.target);
      }
      nodes = nodes.filter((n) => connectedIds.has(n.id));
    }

    return { nodes, edges, total_nodes: data.total_nodes };
  }, [data, enabledLabels, enabledEdgeTypes, isolateByEdge]);

  useEffect(() => {
    if (project) {
      fetchOverview(project, { clusterMode, colorMode, optimize, graphMode });
      setHighlightedIds(null);
      setSelectedPath(null);
    }
  }, [project, clusterMode, colorMode, optimize, graphMode, fetchOverview]);

  const handleSelectPath = useCallback(
    (path: string, nodeIds: Set<number>) => {
      if (!filteredData || !path || nodeIds.size === 0) {
        setHighlightedIds(null);
        setSelectedPath(null);
        setCameraTarget(null);
        return;
      }
      setSelectedPath(path);
      setHighlightedIds(nodeIds);
      setCameraTarget(computeCameraTarget(filteredData.nodes, nodeIds));
    },
    [filteredData],
  );

  const handleNodeClick = useCallback(
    (node: GraphNode) => {
      if (!filteredData) return;
      setSelectedNode(node);

      /* Highlight the node and its direct connections */
      const connectedIds = new Set([node.id]);
      for (const edge of filteredData.edges) {
        if (edge.source === node.id) connectedIds.add(edge.target);
        if (edge.target === node.id) connectedIds.add(edge.source);
      }
      setHighlightedIds(connectedIds);
      setSelectedPath(node.file_path ?? null);
      setCameraTarget(computeCameraTarget(filteredData.nodes, connectedIds));
    },
    [filteredData],
  );

  const handleNavigateToNode = useCallback(
    (node: GraphNode) => {
      handleNodeClick(node);
    },
    [handleNodeClick],
  );

  const toggleLabel = useCallback((label: string) => {
    setEnabledLabels((prev) => {
      const next = new Set(prev);
      if (next.has(label)) next.delete(label);
      else next.add(label);
      return next;
    });
  }, []);

  const toggleEdgeType = useCallback((type: string) => {
    setEnabledEdgeTypes((prev) => {
      const next = new Set(prev);
      if (next.has(type)) next.delete(type);
      else next.add(type);
      return next;
    });
  }, []);

  const enableAll = useCallback(() => {
    if (!data) return;
    setEnabledLabels(new Set(data.nodes.map((n) => n.label)));
    setEnabledEdgeTypes(new Set(data.edges.map((e) => e.type)));
  }, [data]);

  const disableAll = useCallback(() => {
    setEnabledLabels(new Set());
    setEnabledEdgeTypes(new Set());
  }, []);

  if (!project) {
    return (
      <div className="flex items-center justify-center h-full">
        <p className="text-white/30 text-sm">
          Select a project from the Stats tab
        </p>
      </div>
    );
  }

  if (loading) {
    return (
      <div className="flex items-center justify-center h-full">
        <div className="text-center">
          <div className="w-8 h-8 border-2 border-cyan-400/30 border-t-cyan-400 rounded-full animate-spin mx-auto mb-3" />
          <p className="text-white/40 text-sm">Computing layout...</p>
        </div>
      </div>
    );
  }

  if (error) {
    return (
      <div className="flex items-center justify-center h-full">
        <div className="text-center p-8">
          <p className="text-red-400 text-sm mb-2">{error}</p>
          <Button variant="outline" size="sm" onClick={() => fetchOverview(project)}>
            Retry
          </Button>
        </div>
      </div>
    );
  }

  if (!data || !filteredData || filteredData.nodes.length === 0) {
    return (
      <div className="flex items-center justify-center h-full">
        <div className="text-center">
          <p className="text-white/30 text-sm mb-3">
            {data && filteredData?.nodes.length === 0
              ? "All nodes filtered out"
              : "No nodes in this project"}
          </p>
          {data && filteredData?.nodes.length === 0 && (
            <Button size="sm" onClick={enableAll}>
              Reset Filters
            </Button>
          )}
        </div>
      </div>
    );
  }

  return (
    <div className="h-full flex">
      {/* Left sidebar — resizable */}
      <div
        className="border-r border-border/30 flex flex-col h-full bg-[#0b1920]/90 backdrop-blur-md shrink-0"
        style={{ width: leftWidth }}
      >
        <FilterPanel
          data={data}
          enabledLabels={enabledLabels}
          enabledEdgeTypes={enabledEdgeTypes}
          showLabels={showLabels}
          onToggleLabel={toggleLabel}
          onToggleEdgeType={toggleEdgeType}
          onToggleShowLabels={() => setShowLabels((v) => !v)}
          onEnableAll={enableAll}
          onDisableAll={disableAll}
        />
        <Sidebar
          nodes={filteredData.nodes}
          onSelectPath={handleSelectPath}
          selectedPath={selectedPath}
        />
      </div>
      <ResizeHandle
        side="left"
        onResize={(d) => {
          setLeftWidth((w) => {
            const nw = Math.max(150, Math.min(500, w + d));
            saveWidth("cbm-left-w", nw);
            return nw;
          });
        }}
      />

      {/* Graph area */}
      <div className="flex-1 relative overflow-hidden">
        <ErrorBoundary>
          <GraphScene
            data={filteredData}
            highlightedIds={highlightedIds}
            cameraTarget={cameraTarget}
            showLabels={showLabels}
            onNodeClick={handleNodeClick}
          />
        </ErrorBoundary>

        {/* HUD */}
        <div className="absolute top-4 left-4 text-[11px] text-white/30 pointer-events-none font-mono">
          <p>
            {filteredData.nodes.length.toLocaleString()} nodes /{" "}
            {filteredData.edges.length.toLocaleString()} edges
            <span className="text-white/20 ml-2">
              [{clusterMode === "louvain" ? "louvain" : "directory"} clusters]
            </span>
          </p>
          {data.nodes.length > filteredData.nodes.length && (
            <p className="text-white/25 mt-0.5">
              filtered from {data.nodes.length.toLocaleString()}
            </p>
          )}
          {highlightedIds && highlightedIds.size > 0 && (
            <p className="text-cyan-400/50 mt-0.5">
              {highlightedIds.size} selected
            </p>
          )}
        </div>

        <div className="absolute top-4 right-4 flex gap-2 pointer-events-none [&_button]:pointer-events-auto [&_a]:pointer-events-auto">
          {/* Collapse/expand the toggle cluster */}
          <button
            onClick={() => {
              setShowToggles((v) => {
                const next = !v;
                saveBool("cbm-show-toggles", next);
                return next;
              });
            }}
            className="px-2 py-1.5 text-[11px] font-medium rounded-lg border border-border/30 bg-white/[0.03] text-white/50 hover:text-white/80 transition-colors"
            title={showToggles ? "Hide toggles" : "Show toggles"}
          >
            {showToggles ? "⚙ −" : "⚙ +"}
          </button>
          {showToggles && (
            <>
          {/* Graph mode toggle: Raw (all nodes) vs Runtime (excludes Module/File) */}
          <div className="flex rounded-lg overflow-hidden border border-border/30">
            <button
              onClick={() => setGraphMode("raw")}
              className={`px-3 py-1.5 text-[11px] font-medium transition-colors ${
                graphMode === "raw"
                  ? "bg-primary/20 text-primary"
                  : "bg-white/[0.03] text-white/40 hover:text-white/60"
              }`}
              title="Full call graph — no filtering"
            >
              Raw
            </button>
            <button
              onClick={() => setGraphMode("runtime")}
              className={`px-3 py-1.5 text-[11px] font-medium transition-colors ${
                graphMode === "runtime"
                  ? "bg-primary/20 text-primary"
                  : "bg-white/[0.03] text-white/40 hover:text-white/60"
              }`}
              title="Runtime lens — excludes Module and File nodes before clustering"
            >
              Runtime
            </button>
          </div>
          {/* Cluster mode toggle (layout): Directory vs Louvain */}
          <div className="flex rounded-lg overflow-hidden border border-border/30">
            <button
              onClick={() => setClusterMode("dir")}
              className={`px-3 py-1.5 text-[11px] font-medium transition-colors ${
                clusterMode === "dir"
                  ? "bg-primary/20 text-primary"
                  : "bg-white/[0.03] text-white/40 hover:text-white/60"
              }`}
              title="Ring layout by directory prefix"
            >
              Dir
            </button>
            <button
              onClick={() => setClusterMode("louvain")}
              className={`px-3 py-1.5 text-[11px] font-medium transition-colors ${
                clusterMode === "louvain"
                  ? "bg-primary/20 text-primary"
                  : "bg-white/[0.03] text-white/40 hover:text-white/60"
              }`}
              title="Ring layout by Louvain community"
            >
              Louvain
            </button>
          </div>
          {/* Color mode toggle: Stellar (by degree) vs Louvain (by community) */}
          <div className="flex rounded-lg overflow-hidden border border-border/30">
            <button
              onClick={() => setColorMode("stellar")}
              className={`px-3 py-1.5 text-[11px] font-medium transition-colors ${
                colorMode === "stellar"
                  ? "bg-primary/20 text-primary"
                  : "bg-white/[0.03] text-white/40 hover:text-white/60"
              }`}
              title="Color by node degree (stellar spectral palette)"
            >
              ★ Stellar
            </button>
            <button
              onClick={() => setColorMode("louvain")}
              className={`px-3 py-1.5 text-[11px] font-medium transition-colors ${
                colorMode === "louvain"
                  ? "bg-primary/20 text-primary"
                  : "bg-white/[0.03] text-white/40 hover:text-white/60"
              }`}
              title="Color by Louvain community id"
            >
              ◉ Louvain
            </button>
          </div>
          {/* Optimize toggle: run the post-seed local_optimize force pass, or freeze */}
          <div className="flex rounded-lg overflow-hidden border border-border/30">
            <button
              onClick={() => setOptimize(!optimize)}
              className={`px-3 py-1.5 text-[11px] font-medium transition-colors ${
                optimize
                  ? "bg-primary/20 text-primary"
                  : "bg-white/[0.03] text-white/40 hover:text-white/60"
              }`}
              title={
                optimize
                  ? "Force pass is ON — click to freeze seeded positions"
                  : "Force pass is OFF — click to enable gentle force optimization"
              }
            >
              {optimize ? "Force: on" : "Force: off"}
            </button>
          </div>
          {/* Isolate toggle: hide nodes not connected by any visible edge type */}
          <div className="flex rounded-lg overflow-hidden border border-border/30">
            <button
              onClick={() => setIsolateByEdge(!isolateByEdge)}
              className={`px-3 py-1.5 text-[11px] font-medium transition-colors ${
                isolateByEdge
                  ? "bg-primary/20 text-primary"
                  : "bg-white/[0.03] text-white/40 hover:text-white/60"
              }`}
              title={
                isolateByEdge
                  ? "Isolate ON — only showing nodes with visible edges. Click to show all."
                  : "Isolate OFF — all nodes visible. Click to hide nodes without visible edges."
              }
            >
              {isolateByEdge ? "Isolate: on" : "Isolate: off"}
            </button>
          </div>
            </>
          )}
          {highlightedIds && (
            <Button
              size="sm"
              onClick={() => {
                setHighlightedIds(null);
                setSelectedPath(null);
                setSelectedNode(null);
                setCameraTarget(null);
              }}
            >
              Clear
            </Button>
          )}
          <Button
            variant="outline"
            size="sm"
            onClick={() => {
              setCameraTarget(computeFitAllTarget(filteredData.nodes));
            }}
            title="Frame the entire graph in view"
          >
            Fit
          </Button>
          <Button
            variant="outline"
            size="sm"
            onClick={() => {
              setHighlightedIds(null);
              setSelectedPath(null);
              setSelectedNode(null);
              setCameraTarget(null);
              fetchOverview(project, { clusterMode, colorMode, optimize, graphMode });
            }}
          >
            Refresh
          </Button>
        </div>
      </div>

      {/* Right detail panel — resizable */}
      {selectedNode && filteredData && (
        <>
          <ResizeHandle
            side="right"
            onResize={(d) => {
              setRightWidth((w) => {
                const nw = Math.max(200, Math.min(500, w + d));
                saveWidth("cbm-right-w", nw);
                return nw;
              });
            }}
          />
          <div
            className="border-l border-border shrink-0 h-full overflow-hidden"
            style={{ width: rightWidth, maxHeight: "100%" }}
          >
            <NodeDetailPanel
              node={selectedNode}
              allNodes={filteredData.nodes}
              allEdges={filteredData.edges}
              onClose={() => {
                setSelectedNode(null);
                setHighlightedIds(null);
                setSelectedPath(null);
              }}
              onNavigate={handleNavigateToNode}
            />
          </div>
        </>
      )}
    </div>
  );
}
