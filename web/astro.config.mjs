import { defineConfig } from 'astro/config';
import tailwindcss from '@tailwindcss/vite';

export default defineConfig({
  site: 'https://vorlie.github.io',
  base: '/iota-studio',
  trailingSlash: 'always',
  vite: {
    plugins: [tailwindcss()],
  },
});
