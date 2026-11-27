export function withBase(path: string): string {
  const base = import.meta.env.BASE_URL.replace(/\/+$/, '');
  if (!path || path === '/') {
    return `${base}/`;
  }
  const clean = path.startsWith('/') ? path : `/${path}`;
  return `${base}${clean}`;
}
