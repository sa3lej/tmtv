/** @type {import('tailwindcss').Config} */
export default {
  content: ['./src/**/*.{astro,html,js,jsx,md,mdx,svelte,ts,tsx,vue}'],
  theme: {
    extend: {
      fontFamily: {
        mono: ['"JetBrains Mono"', '"Fira Code"', '"SF Mono"', 'Menlo', 'monospace'],
        sans: ['Inter', '-apple-system', 'sans-serif'],
      },
      keyframes: {
        blink: {
          '0%, 100%': { opacity: '1' },
          '50%': { opacity: '0' },
        },
        'fade-up': {
          '0%': { opacity: '0', transform: 'translateY(8px)' },
          '100%': { opacity: '1', transform: 'translateY(0)' },
        },
      },
      animation: {
        blink: 'blink 1.1s step-end infinite',
        'fade-up': 'fade-up 0.5s ease-out both',
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
