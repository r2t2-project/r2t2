class JobInfo {
  constructor(job_id, bucket, region) {
    this.job_id = job_id;
    this.bucket = bucket;
    this.region = region;
  }

  tile_url(tile_id) {
    return `http://${this.bucket}.s3.${this.region}.amazonaws.com/jobs/${this.job_id}/out/${tile_id}.png`;
  }
}

class TileHelper {
  constructor(width, height, tile_count) {
    this.width = width;
    this.height = height;
    this.tile_count = tile_count;

    let tile_size = Math.ceil(Math.sqrt(width * height / tile_count));

    while (Math.ceil(1.0 * width / tile_size)
      * Math.ceil(1.0 * height / tile_size)
      > tile_count) {
      tile_size++;
    }

    this.tile_size = tile_size;
    console.log(this.tile_count);
    console.log(this.tile_size);

    this.n_tiles = {
      x: Math.ceil(1.0 * this.width / this.tile_size),
      y: Math.ceil(1.0 * this.height / this.tile_size)
    };
  }

  bounds(tile_id) {
    const tile_x = tile_id % this.n_tiles.x;
    const tile_y = Math.floor(tile_id / this.n_tiles.x);

    const x0 = tile_x * this.tile_size;
    const x1 = Math.min(x0 + this.tile_size, this.width);
    const y0 = tile_y * this.tile_size;
    const y1 = Math.min(y0 + this.tile_size, this.height);

    return {
      x: x0, y: y0,
      w: x1 - x0, h: y1 - y0
    };
  }
}

const url_params = new URLSearchParams(window.location.search);

const _job = new JobInfo(url_params.get('job_id'),
  url_params.get('bucket'),
  url_params.get('region'));

const _tiles = new TileHelper(parseInt(url_params.get('width')),
  parseInt(url_params.get('height')),
  parseInt(url_params.get('tiles')));

let svg = document.querySelector('#output');

for (let i = 0; i < _tiles.n_tiles.x * _tiles.n_tiles.y; i++) {
  const bounds = _tiles.bounds(i);
  const url = _job.tile_url(i);

  let element = document.createElementNS('http://www.w3.org/2000/svg', 'image');
  element.setAttributeNS(null, 'x', bounds.x);
  element.setAttributeNS(null, 'y', bounds.y);
  element.setAttributeNS(null, 'width', bounds.w);
  element.setAttributeNS(null, 'height', bounds.h);
  element.setAttributeNS(null, 'href', url);
  svg.appendChild(element);
}
