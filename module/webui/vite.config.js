import { defineConfig } from 'vite';
import { createHtmlPlugin } from 'vite-plugin-html'

export default defineConfig({
    base: './',
    build: {
        outDir: '../webroot',
    },
    plugins: [
        createHtmlPlugin({
            minify: true
        })
    ]
});
