import createQTSQModule from './qtsq-decoder.js';

export class QTSQDecoder {
    constructor() {
        this.module = null;
        this.isReady = false;
        this.initPromise = null;
    }

    async init() {
        if (this.isReady) return true;
        if (this.initPromise) return this.initPromise;

        this.initPromise = (async () => {
            try {
                this.module = await createQTSQModule();
                this.isReady = true;
                return true;
            } catch (e) {
                console.error("Failed to initialize QTSQ WASM module", e);
                throw e;
            }
        })();

        return this.initPromise;
    }

    async decode(arrayBuffer, password = "") {
        await this.init();
        
        const data = new Uint8Array(arrayBuffer);
        const len = data.length;
        
        // Allocate memory inside WASM for input data
        const dataPtr = this.module._malloc(len);
        this.module.HEAPU8.set(data, dataPtr);
        
        // Allocate memory for password string
        let pwdPtr = 0;
        if (password) {
            const pwdBytes = new TextEncoder().encode(password + '\0');
            pwdPtr = this.module._malloc(pwdBytes.length);
            this.module.HEAPU8.set(pwdBytes, pwdPtr);
        }
        
        // Call decode
        const res = this.module.ccall(
            'qtsq_wasm_decode',
            'number',
            ['number', 'number', 'number'],
            [dataPtr, len, pwdPtr]
        );
        
        // Free inputs
        this.module._free(dataPtr);
        if (pwdPtr) this.module._free(pwdPtr);
        
        if (res !== 0) {
            throw new Error(`QTSQ Decode Error: ${res}`);
        }
        
        const type = this.module.ccall('qtsq_wasm_get_type', 'number', [], []);
        const width = this.module.ccall('qtsq_wasm_get_width', 'number', [], []);
        const height = this.module.ccall('qtsq_wasm_get_height', 'number', [], []);
        const size = this.module.ccall('qtsq_wasm_get_pixels_size', 'number', [], []);
        
        const isImage = (type === 0x10 || type === 0x11 || type === 0x12);
        const isText = (type === 0x40 || type === 0x41 || type === 0x52 || type === 0x53);
        
        let result = {
            type,
            width,
            height,
            isImage,
            isText,
            pixels: null,
            text: null,
            isPassthrough: false
        };
        
        if (isImage) {
            const pxPtr = this.module.ccall('qtsq_wasm_get_pixels', 'number', [], []);
            if (pxPtr !== 0) {
                // Copy data out of WASM memory before it gets freed or overwritten
                const pixelData = new Uint8Array(this.module.HEAPU8.buffer, pxPtr, size);
                
                if (width === 0 || height === 0) {
                    // JPEG/PNG passthrough
                    result.pixels = new Uint8Array(pixelData);
                    result.isPassthrough = true;
                } else {
                    // RGBA array for Canvas ImageData
                    result.pixels = new Uint8ClampedArray(pixelData);
                }
            }
        } else if (isText) {
            const txtPtr = this.module.ccall('qtsq_wasm_get_text', 'number', [], []);
            if (txtPtr !== 0) {
                const textData = new Uint8Array(this.module.HEAPU8.buffer, txtPtr, size);
                result.text = new TextDecoder().decode(textData);
            }
        }
        
        return result;
    }
    
    async isEncrypted(arrayBuffer) {
        await this.init();
        
        const data = new Uint8Array(arrayBuffer);
        const len = data.length;
        if (len < 112) return false; // Smaller than minimum header size
        
        const dataPtr = this.module._malloc(len);
        this.module.HEAPU8.set(data, dataPtr);
        
        const res = this.module.ccall(
            'qtsq_wasm_is_encrypted',
            'number',
            ['number', 'number'],
            [dataPtr, len]
        );
        
        this.module._free(dataPtr);
        return res === 1;
    }
    
    free() {
        if (this.module && this.isReady) {
            this.module.ccall('qtsq_wasm_free', 'void', [], []);
        }
    }
}
