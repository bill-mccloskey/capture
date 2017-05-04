const kReuseScanline = 51;
const kNewScanline = 122;

function sendRequest(url) {
  return new Promise((resolve) => {
    let req = new XMLHttpRequest();
    req.onload = (event) => {
      if (req.response) {
        resolve(req.response);
      }
    };
    req.open("GET", url);
    req.responseType = "arraybuffer";
    req.send();
  });
}

function Decoder(popBuffer, idxBuffer) {
  this.popBuffer = popBuffer;
  this.idxBuffer = idxBuffer;

  this.readIndex();

  this.input = new Uint8Array(this.popBuffer);
}

Decoder.prototype.readIndex = function() {
  let array16 = new Uint16Array(this.idxBuffer);
  let width = array16[0];
  let height = array16[1];

  this.width = width;
  this.height = height;
  this.index = new Array();

  let array32 = new Uint32Array(this.idxBuffer);
  for (let i = 1; i < array32.length; i += 2) {
    this.index.push(array32[i]);
  }

  this.numFrames = this.index.length;
};

Decoder.prototype.runLengthDecode = function(inOffset) {
  let output = this.output;
  let outStart = this.outOffset;
  let outOffset = this.outOffset;
  let width = this.width * 4;
  let input = this.input;

  while (outOffset - outStart < width) {
    let count = input[inOffset++];
    let b = input[inOffset++];

    for (; count; count--) {
      // RGBA
      output[outOffset++] = b;
      output[outOffset++] = b;
      output[outOffset++] = b;
      output[outOffset++] = 255;
    }
  }

  this.outOffset = outOffset;
  return inOffset;
};

Decoder.prototype.readUint64 = function(inOffset) {
  let x1 = this.input[inOffset + 0];
  let x2 = this.input[inOffset + 1];
  let x3 = this.input[inOffset + 2];
  let x4 = this.input[inOffset + 3];
  let x5 = this.input[inOffset + 4];
  let x6 = this.input[inOffset + 5];
  let x7 = this.input[inOffset + 6];

  // Don't include the high bits since JS can't represent them anyway.
  return (x1 << (8*0)) |
         (x2 << (8*1)) |
         (x3 << (8*2)) |
         (x4 << (8*3)) |
         (x5 << (8*4)) |
         (x6 << (8*5)) |
         (x7 << (8*6));
};

Decoder.prototype.readScanline = function(inOffset) {
  if (this.input[inOffset] == kNewScanline) {
    inOffset++;
    inOffset = this.runLengthDecode(inOffset);
  } else if (this.input[inOffset] == kReuseScanline) {
    inOffset++;
    let innerOffset = this.readUint64(inOffset);
    inOffset += 8;

    this.readScanline(innerOffset);
  } else {
    throw "ERROR";
  }

  return inOffset;
};

Decoder.prototype.decodeFrame = function(frameIndex, output) {
  this.outOffset = 0;
  this.output = output;

  let inOffset = this.index[frameIndex];
  let height = this.height;
  for (let h = 0; h < height; h++) {
    inOffset = this.readScanline(inOffset);
  }
};

function start(decoder) {
  let progressElt = document.getElementById("progress");
  let statusElt = document.getElementById("status");
  let canvas = document.getElementById("canvas");
  let ctx = canvas.getContext("2d");
  let imageData = ctx.createImageData(decoder.width, decoder.height);

  progressElt.setAttribute("max", decoder.numFrames - 1);

  let frameIndex = 0;
  let playing = true;

  function draw() {
    decoder.decodeFrame(frameIndex, imageData.data);
    ctx.putImageData(imageData, 0, 0);

    progressElt.setAttribute("value", frameIndex);
    statusElt.innerHTML = (frameIndex + 1) + "/" + decoder.numFrames;
  }

  function playOne() {
    if (!playing) {
      return;
    }

    if (frameIndex == decoder.numFrames - 1) {
      playing = false;
      return;
    }

    frameIndex++;
    draw();

    requestAnimationFrame(playOne);
  }
  requestAnimationFrame(playOne);

  document.addEventListener("keypress", (event) => {
    if (event.key == "ArrowRight" && frameIndex < decoder.numFrames - 1) {
      frameIndex++;
      draw();
      event.preventDefault();
    } else if (event.key == "ArrowLeft" && frameIndex > 0) {
      frameIndex--;
      draw();
      event.preventDefault();
    } else if (event.key == " ") {
      if (!playing && frameIndex == decoder.numFrames - 1) {
        return;
      }
      playing = !playing;
      if (playing) {
        requestAnimationFrame(playOne);
      }
      event.preventDefault();
    }
  });

  progressElt.addEventListener("click", (event) => {
    let percent = (event.pageX  - (progressElt.offsetLeft + progressElt.offsetParent.offsetLeft)) / progressElt.offsetWidth;
    frameIndex = Math.floor(percent * decoder.numFrames);
    playing = false;
    draw();
  });
}

Promise.all([sendRequest("video.pop"), sendRequest("video.idx")]).then(
  ([pop, idx]) => {
    let decoder = new Decoder(pop, idx);
    start(decoder);
  });
