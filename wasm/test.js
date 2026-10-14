import fs from 'fs';
import { QTSQDecoder } from './src/qtsq-decoder.js';

async function main() {
    console.log("Starting decoder test...");
    try {
        const decoder = new QTSQDecoder();
        await decoder.init();
        
        // Read a test QTSQ file if available
        const testFile = '../test_cpu.qtsq';
        if (fs.existsSync(testFile)) {
            const buffer = fs.readFileSync(testFile);
            
            console.log("Decoding test_cpu.qtsq...");
            const isEncrypted = await decoder.isEncrypted(buffer.buffer);
            console.log(`Is Encrypted: ${isEncrypted}`);
            
            const result = await decoder.decode(buffer.buffer, "");
            console.log(`Type: 0x${result.type.toString(16)}`);
            console.log(`Dimensions: ${result.width}x${result.height}`);
            console.log(`Is Image: ${result.isImage}`);
            console.log(`Is Text: ${result.isText}`);
            console.log(`Passthrough: ${result.isPassthrough}`);
            
            if (result.pixels) {
                console.log(`Got pixels! Length: ${result.pixels.length}`);
                console.log(`First 4 bytes (RGBA): ${result.pixels[0]}, ${result.pixels[1]}, ${result.pixels[2]}, ${result.pixels[3]}`);
            }
            if (result.text) {
                console.log(`Got text! Length: ${result.text.length}`);
                console.log(`Text snippet: ${result.text.substring(0, 50)}...`);
            }
        } else {
            console.log("No test.qtsq found to decode.");
        }
        
        decoder.free();
        console.log("Test completed successfully!");
    } catch (e) {
        console.error("Test failed:", e);
    }
}

main();
