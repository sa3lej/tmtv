/** @type {import('tailwindcss').Config} */
export default {
  content: ['./src/**/*.{astro,html,js,jsx,md,mdx,svelte,ts,tsx,vue}'],
  theme: {
    extend: {
      fontFamily: {
        mono: ['"JetBrains Mono"', '"Fira Code"', '"SF Mono"', 'Menlo', 'monospace'],
        sans: ['Inter', '-apple-system', 'sans-serif'],
      },
      colors: {
        tmtv: {
          bg: '#0c0c16',
          surface: 'rgba(255, 255, 255, 0.03)',
          border: 'rgba(255, 255, 255, 0.06)',
          blue: '#6c9efc',
          purple: '#c084fc',
          green: '#3ddc84',
          red: '#ff6b6b',
        },
      },
    },
  },
  plugins: [],
};
