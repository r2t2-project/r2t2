class JobInfo {
  constructor(job_id, bucket, region) {
    this.job_id = job_id;
    this.bucket = bucket;
    this.region = region;
  }

  tile_version_url(tile_id) {
    return `http://${this.bucket}.s3.${this.region}.amazonaws.com/jobs/${this.job_id}/out/${tile_id}`;
  }

  tile_url(tile_id, version) {
    return `http://${this.bucket}.s3.${this.region}.amazonaws.com/jobs/${this.job_id}/out/${tile_id}-${version}.png`;
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

let ctx = document.getElementById("output").getContext('2d');

let _tile_versions = new Array(_tiles.n_tiles.x * _tiles.n_tiles.y).fill(-1);

let load_image = (url, x, y, w, h) => {
  let img = new Image();

  img.onload = () => {
    ctx.fillStyle = 'rgba(255, 0, 0, 0.1)';
    ctx.fillRect(x, y, w, h);
    setTimeout(() => {
      ctx.drawImage(img, x, y, w, h);
    }, 500);
  };

  img.src = url;
};

function randint(min, max) {
  min = Math.ceil(min);
  max = Math.floor(max);
  return Math.floor(Math.random() * (max - min) + min);
}

let get_version = (tile_id, url) => {
  let xhr = new XMLHttpRequest();

  xhr.onreadystatechange = () => {
    if (xhr.readyState == XMLHttpRequest.DONE) {
      if (xhr.status == 200) {
        const new_ver = parseInt(xhr.responseText);
        if (new_ver > _tile_versions[tile_id]) {
          _tile_versions[tile_id] = new_ver;
          const bounds = _tiles.bounds(tile_id);
          const tile_url = _job.tile_url(tile_id, new_ver);
          load_image(tile_url, bounds.x, bounds.y, bounds.w, bounds.h);
        }
      }

      setTimeout(() => get_version(tile_id, url), 2000 + randint(-750, 750));
    }
  };

  xhr.open('GET', url + "?d=" + new Date().getTime());
  xhr.send(null);
}

for (let i = 0; i < _tiles.n_tiles.x * _tiles.n_tiles.y; i++) {
  const bounds = _tiles.bounds(i);
  const tile_url = _job.tile_url(i);
  const tile_ver_url = _job.tile_version_url(i);

  get_version(i, tile_ver_url);
}
