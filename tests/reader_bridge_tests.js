// Exercise the production bridge with controlled content/layout promises.
// No browser, windows, network, or elapsed-time assumptions.
const fs=require('node:fs'),vm=require('node:vm'),assert=require('node:assert/strict');
const html=fs.readFileSync(process.argv[2],'utf8');
const source=html.slice(html.indexOf("'use strict';"),html.lastIndexOf('</script>'));
const messages=[],frames=[];
const context=vm.createContext({
  chrome:{webview:{postMessage:value=>messages.push(value),addEventListener(){}}},
  document:{addEventListener(){}},window:{addEventListener(){}},
  requestAnimationFrame:callback=>frames.push(callback),innerWidth:800,innerHeight:600,
});
vm.runInContext(source,context);
async function flush(){for(let i=0;i<30;i++){await Promise.resolve();for(const callback of frames.splice(0))callback();}}
async function run(){
  const node=(localName,values)=>({localName,attributes:Object.entries(values).map(([name,value])=>({name,value})),
    removeAttribute(name){this.attributes=this.attributes.filter(attribute=>attribute.name!==name);}});
  const svgImage=node('image',{'href':'data:image/png;base64,AAAA','xlink:href':'DATA:IMAGE/PNG;BASE64,BBBB','onload':'bad()'});
  const unsafeSvgImage=node('image',{'href':'https://example.invalid/picture.png'});
  const linkedText=node('a',{'href':'data:image/png;base64,CCCC'});
  const bitmap=node('img',{'src':'data:image/jpeg;base64,DDDD','poster':'javascript:bad()'});
  const removed={remove(){this.wasRemoved=true;}};
  context.testRendered={querySelectorAll(selector){return selector==='*'?[svgImage,unsafeSvgImage,linkedText,bitmap]:[removed];}};
  vm.runInContext('sanitizeRenderedDocument(testRendered)',context);
  assert.equal(removed.wasRemoved,true,'active document elements are removed');
  assert.deepEqual(svgImage.attributes.map(attribute=>attribute.name),['href','xlink:href'],'embedded VML image data survives');
  assert.deepEqual(unsafeSvgImage.attributes,[],'external SVG image is removed');
  assert.deepEqual(linkedText.attributes,[],'document links remain disabled');
  assert.deepEqual(bitmap.attributes.map(attribute=>attribute.name),['src'],'embedded bitmap data survives');
  messages.length=0;
  vm.runInContext(`let resolvePage,resolveImage,resolveLayout;
    const testDocument={images:[{decode:()=>new Promise(resolve=>resolveImage=resolve)}],fonts:{ready:Promise.resolve()}};
    rendition={next:()=>new Promise(resolve=>resolvePage=resolve),prev:()=>Promise.resolve(),
      getContents:()=>[{document:testDocument}],resize:()=>{},display:()=>new Promise(resolve=>resolveLayout=resolve)};
    operation('101','next');`,context);
  await flush();assert.deepEqual(messages,[],'dispatch is not completion');
  vm.runInContext('resolvePage()',context);await flush();assert.deepEqual(messages,[],'wait for image decode');
  vm.runInContext('resolveImage()',context);await flush();assert.deepEqual(messages,['done:101']);
  messages.length=0;
  vm.runInContext("testDocument.images=[];operation('102','resize');operation('103','previous');",context);
  await flush();assert.deepEqual(messages,[],'resize waits for EPUB display');
  vm.runInContext('resolveLayout()',context);await flush();assert.deepEqual(messages,['done:102','done:103'],'operations retain identity and order');
  messages.length=0;
  vm.runInContext("rendition.next=()=>Promise.reject(new Error('layout failed'));operation('104','next');",context);
  await flush();assert.deepEqual(messages,['failed:104']);
  messages.length=0;
  vm.runInContext(`const root={scrollTop:0,scrollHeight:2000};
    htmlFrame={clientHeight:600,contentDocument:testDocument,contentWindow:{innerHeight:600,document:{scrollingElement:root},scrollBy:(x,y)=>root.scrollTop+=y}};
    operation('105','next');`,context);
  await flush();assert(messages.includes('done:105'),'HTML/DOCX page completion');
  assert.equal(vm.runInContext('root.scrollTop',context),576);
  messages.length=0;vm.runInContext("operation('106','resize')",context);await flush();assert(messages.includes('done:106'),'HTML/DOCX layout completion');
  console.log('PASS: EPUB and HTML/DOCX completion, image decode, operation queue and errors');
}
run().catch(error=>{console.error(error);process.exitCode=1;});
