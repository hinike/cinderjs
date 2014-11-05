//
// Cubes Example
var gl = require('gl');
var Camera = require('camera');
var Shader = require('shader');
var Batch = require('batch');

app.addAssetDirectory(__dirname + '/assets/');

// Screen size holder
var ctxSize = { x: 640, y: 480 };

// Mouse
var mouse = { x: 0, y: 0 };

// Rotation
var rotation = { x: 0, y: 0, z: 0 };

var drawMethod = 0;
var wireframe = false;

var cam = new Camera();
cam.lookAt( 3, 2, 4, 0, 0, 0 );
cam.setPerspective( 60.0, app.getAspectRatio(), 1.0, 1000.0 );

var glslProg = new Shader( 'shader.vert', 'shader.frag' );
var batch = new Batch(0, glslProg);

// GL Draw loop
var loop = function(timePassed, mx, my){
  gl.enableDepthRead();
  gl.enableDepthWrite();

  // Update mouse position
  mouse.x = mx;
  mouse.y = my;
  
  gl.pushMatrices();
  
  gl.clear( 0.1, 0.1, 0.11 );
  
  // Update rotation
  rotation.y += 0.6;
  rotation.z += 0.8;
  
  gl.setMatrices(cam.id);

  if(wireframe) gl.enableWireframe();
  
  batch.draw();

  if(wireframe) gl.disableWireframe();

  gl.popMatrices();

  gl.disableDepthRead();
  gl.disableDepthWrite();

}

// Register draw loop (executed each frame, allows drawing to window)
app.draw(loop);

app.on('resize', function(w, h){
  ctxSize.x = w;
  ctxSize.y = h;

  cam.setPerspective( 60.0, app.getAspectRatio(), 1.0, 1000.0 );
});

// Handle key events
app.on('keydown', function( evt ){
  if(evt.charCode == 99) { // C
    toggleAppConsole();
  } else if(evt.charCode == 27) { // ESC
    quit();
  } else if(evt.charCode == 118) { // V
    toggleV8Stats();
  } else if(evt.charCode == 119) { // W
    wireframe = !wireframe;
  }

});