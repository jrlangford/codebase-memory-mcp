/**
 * Extract a short display name from a full project key.
 *
 * Project keys are hyphen-encoded paths like:
 *   home-jonathan-team-agents-cicada-repos-cicada_backend
 *   home-jonathan-team-workspace-shared-platform-cicada_backend
 *
 * We look for known separators and return what follows.
 */
export function shortProjectName(name: string): string {
  for (const sep of ["-repos-", "-platform-"]) {
    const idx = name.lastIndexOf(sep);
    if (idx >= 0) return name.slice(idx + sep.length);
  }
  return name;
}
