import { useCallback, useState } from "react";
import type { GraphData } from "../lib/types";

export type ClusterMode = "dir" | "louvain";
export type ColorMode = "stellar" | "louvain";
export type GraphMode = "raw" | "runtime";

/* Node labels dropped under each graph mode. "raw" keeps everything;
 * "runtime" drops Module+File scaffolding so the graph represents
 * functions, methods, classes, routes, and their CALLS edges. */
const EXCLUDE_BY_MODE: Record<GraphMode, string> = {
  raw: "",
  runtime: "Module,File",
};

export interface LayoutOptions {
  clusterMode?: ClusterMode;
  colorMode?: ColorMode;
  optimize?: boolean;
  graphMode?: GraphMode;
}

interface UseGraphDataResult {
  data: GraphData | null;
  loading: boolean;
  error: string | null;
  fetchOverview: (project: string, options?: LayoutOptions) => void;
  fetchDetail: (project: string, centerNode: string) => void;
}

async function fetchLayout(
  project: string,
  maxNodes = 50000,
  options: LayoutOptions = {},
): Promise<GraphData> {
  const {
    clusterMode = "louvain",
    colorMode = "stellar",
    optimize = false,
    graphMode = "raw",
  } = options;
  const params = new URLSearchParams({
    project,
    max_nodes: String(maxNodes),
    cluster: clusterMode,
    color: colorMode,
    optimize: optimize ? "true" : "false",
  });
  const exclude = EXCLUDE_BY_MODE[graphMode];
  if (exclude) {
    params.set("exclude_node_types", exclude);
  }
  const res = await fetch(`/api/layout?${params}`);

  if (!res.ok) {
    const body = await res.json().catch(() => ({ error: res.statusText }));
    throw new Error(body.error ?? `HTTP ${res.status}`);
  }

  return res.json();
}

export function useGraphData(): UseGraphDataResult {
  const [data, setData] = useState<GraphData | null>(null);
  const [loading, setLoading] = useState(false);
  const [error, setError] = useState<string | null>(null);

  const fetchOverview = useCallback(
    async (project: string, options: LayoutOptions = {}) => {
      setLoading(true);
      setError(null);
      try {
        const result = await fetchLayout(project, 50000, options);
        setData(result);
      } catch (e) {
        setError(e instanceof Error ? e.message : "Failed to fetch layout");
      } finally {
        setLoading(false);
      }
    },
    [],
  );

  const fetchDetail = useCallback(
    async (project: string, _centerNode: string) => {
      setLoading(true);
      setError(null);
      try {
        /* TODO: detail level with center_node filtering */
        const result = await fetchLayout(project, 50000);
        setData(result);
      } catch (e) {
        setError(e instanceof Error ? e.message : "Failed to fetch layout");
      } finally {
        setLoading(false);
      }
    },
    [],
  );

  return { data, loading, error, fetchOverview, fetchDetail };
}
