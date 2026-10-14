import { QTSQDecoder } from '../../wasm/src/qtsq-decoder.js';

class QTSQViewer extends HTMLElement {
    constructor() {
        super();
        this.attachShadow({ mode: 'open' });
        this.decoder = new QTSQDecoder();
        
        // Setup initial DOM
        this.shadowRoot.innerHTML = `
            <style>
                :host {
                    display: inline-block;
                    position: relative;
                    min-width: 200px;
                    min-height: 100px;
                    background: var(--qtsq-bg, #f3f4f6);
                    border-radius: var(--qtsq-radius, 8px);
                    overflow: hidden;
                    font-family: system-ui, -apple-system, sans-serif;
                }
                :host([theme="dark"]) {
                    background: var(--qtsq-bg, #1f2937);
                    color: #fff;
                }
                .container {
                    display: flex;
                    align-items: center;
                    justify-content: center;
                    width: 100%;
                    height: 100%;
                }
                canvas {
                    max-width: 100%;
                    max-height: 100%;
                    object-fit: contain;
                }
                .text-content {
                    padding: 1rem;
                    white-space: pre-wrap;
                    word-break: break-word;
                    width: 100%;
                    height: 100%;
                    overflow: auto;
                    box-sizing: border-box;
                    font-family: monospace;
                    font-size: 14px;
                    background: inherit;
                    color: inherit;
                }
                .loader {
                    width: 24px;
                    height: 24px;
                    border: 3px solid var(--qtsq-loader-color, #9ca3af);
                    border-bottom-color: transparent;
                    border-radius: 50%;
                    animation: spin 1s linear infinite;
                }
                .error {
                    color: #ef4444;
                    padding: 1rem;
                    text-align: center;
                }
                .password-form {
                    display: flex;
                    flex-direction: column;
                    gap: 0.5rem;
                    padding: 1.5rem;
                    background: var(--qtsq-bg, #ffffff);
                    border-radius: 8px;
                    box-shadow: 0 4px 6px -1px rgba(0, 0, 0, 0.1);
                    color: #111827;
                }
                :host([theme="dark"]) .password-form {
                    background: #374151;
                    color: #fff;
                }
                .password-form input {
                    padding: 0.5rem;
                    border: 1px solid #d1d5db;
                    border-radius: 4px;
                    background: transparent;
                    color: inherit;
                }
                .password-form button {
                    padding: 0.5rem 1rem;
                    background: #3b82f6;
                    color: white;
                    border: none;
                    border-radius: 4px;
                    cursor: pointer;
                }
                .password-form button:hover {
                    background: #2563eb;
                }
                @keyframes spin {
                    to { transform: rotate(360deg); }
                }
            </style>
            <div class="container" id="container"></div>
        `;
        this.container = this.shadowRoot.getElementById('container');
    }

    static get observedAttributes() {
        return ['src', 'password'];
    }

    attributeChangedCallback(name, oldValue, newValue) {
        if (oldValue !== newValue) {
            if (name === 'src' && newValue) {
                this.load(newValue, this.getAttribute('password') || "");
            }
        }
    }

    connectedCallback() {
        if (this.hasAttribute('src')) {
            this.load(this.getAttribute('src'), this.getAttribute('password') || "");
        }
    }

    disconnectedCallback() {
        this.decoder.free();
    }

    showLoader() {
        this.container.innerHTML = '<div class="loader"></div>';
    }

    showError(msg) {
        this.container.innerHTML = `<div class="error">⚠️ ${msg}</div>`;
    }

    showPasswordPrompt(src) {
        this.container.innerHTML = `
            <form class="password-form" id="pwdForm">
                <div>🔒 Encrypted QTSQ</div>
                <input type="password" id="pwdInput" placeholder="Enter password..." required />
                <button type="submit">Decrypt</button>
            </form>
        `;
        const form = this.shadowRoot.getElementById('pwdForm');
        form.addEventListener('submit', (e) => {
            e.preventDefault();
            const pwd = this.shadowRoot.getElementById('pwdInput').value;
            this.setAttribute('password', pwd);
            this.load(src, pwd);
        });
    }

    async load(src, password) {
        try {
            this.showLoader();
            
            // Fetch the file
            const response = await fetch(src);
            if (!response.ok) throw new Error(`HTTP ${response.status}`);
            const buffer = await response.arrayBuffer();
            
            // Check encryption first without needing password
            const isEncrypted = await this.decoder.isEncrypted(buffer);
            if (isEncrypted && !password) {
                this.showPasswordPrompt(src);
                return;
            }

            // Decode
            const result = await this.decoder.decode(buffer, password);
            
            this.renderResult(result);
        } catch (e) {
            console.error("QTSQ Viewer Error:", e);
            if (e.message.includes("-9") || e.message.includes("-8")) {
                // Decryption error
                this.container.innerHTML = `
                    <div class="password-form">
                        <div class="error" style="padding:0">❌ Incorrect Password</div>
                        <input type="password" id="pwdInput" placeholder="Try again..." required />
                        <button id="retryBtn">Retry</button>
                    </div>
                `;
                this.shadowRoot.getElementById('retryBtn').addEventListener('click', () => {
                    const pwd = this.shadowRoot.getElementById('pwdInput').value;
                    this.load(src, pwd);
                });
            } else {
                this.showError(e.message);
            }
        }
    }

    renderResult(result) {
        this.container.innerHTML = '';
        
        if (result.isImage) {
            const canvas = document.createElement('canvas');
            canvas.width = result.width;
            canvas.height = result.height;
            const ctx = canvas.getContext('2d');
            
            if (result.isPassthrough) {
                // Blob image (JPEG/PNG stored inside QTSQ)
                const blob = new Blob([result.pixels]);
                const url = URL.createObjectURL(blob);
                const img = new Image();
                img.onload = () => {
                    ctx.drawImage(img, 0, 0);
                    URL.revokeObjectURL(url);
                };
                img.src = url;
            } else {
                // Raw RGBA pixels
                const imgData = new ImageData(result.pixels, result.width, result.height);
                ctx.putImageData(imgData, 0, 0);
            }
            this.container.appendChild(canvas);
            
        } else if (result.isText) {
            const pre = document.createElement('pre');
            pre.className = 'text-content';
            pre.textContent = result.text;
            this.container.appendChild(pre);
            
        } else {
            this.showError("Unsupported QTSQ data type for web viewer.");
        }
    }
}

customElements.define('qtsq-viewer', QTSQViewer);
