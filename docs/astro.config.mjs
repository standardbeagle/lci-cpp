import { defineConfig } from 'astro/config';
import starlight from '@astrojs/starlight';

export default defineConfig({
  site: 'https://dev.standardbeagle.com',
  base: '/lci-cpp',
  integrations: [
    starlight({
      title: 'lci',
      description: 'Sub-millisecond semantic code search over large corpora. 13 languages, CLI + HTTP + Unix socket + MCP server for AI assistants.',
      social: [
        { icon: 'github', label: 'GitHub', href: 'https://github.com/standardbeagle/lci-cpp' },
      ],
      head: [
        {
          tag: 'meta',
          attrs: { property: 'og:title', content: 'lci — Lightning Code Index' },
        },
        {
          tag: 'meta',
          attrs: { property: 'og:description', content: 'Sub-millisecond semantic code search over large corpora. 13 languages, CLI + HTTP + Unix socket + MCP server for AI assistants.' },
        },
        {
          tag: 'meta',
          attrs: { property: 'og:type', content: 'website' },
        },
        {
          tag: 'meta',
          attrs: { property: 'og:url', content: 'https://dev.standardbeagle.com/lci-cpp/' },
        },
        {
          tag: 'meta',
          attrs: { name: 'twitter:card', content: 'summary' },
        },
      ],
      customCss: ['./src/styles/custom.css'],
      sidebar: [
        {
          label: 'Start Here',
          items: [
            { label: 'Getting Started', slug: 'getting-started' },
          ],
        },
        {
          label: 'Reference',
          items: [
            { label: 'CLI Usage', slug: 'cli-usage' },
            { label: 'MCP Server', slug: 'mcp-server' },
            { label: 'Error-Handling Score', slug: 'error-handling-score' },
            { label: 'HTTP & Socket API', slug: 'http-socket-api' },
          ],
        },
      ],
    }),
  ],
});
