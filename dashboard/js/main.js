"use strict";

var _state = {
  primary_plot: "rays_enqueued",
  secondary_plot: "paths",
  split_view: true,
  markers: {
    start: true,
    done_95: true,
    done_99: true
  }
};

var completion_time = (data, f, total_paths) => {
  return d3.bisector(d => +d.runningCompletion)
    .right(data, f * total_paths);
};

var metrics = (info) => {
  return {
    paths: {
      label: "Paths Finished (%)",
      func: (d) => (100 * d.runningCompletion / +info.totalPaths),
      format: "d"
    },
    rays_enqueued: {
      label: "Rays Enqueued",
      func: (d) => d.raysEnqueued,
      format: "~s"
    },
    rays_dequeued: {
      label: "Rays Dequeued",
      func: (d) => d.raysDequeued,
      format: "~s"
    },
    bags: {
      label: "Average Bag Size (bytes)",
      func: (d) => d.bagsEnqueued,
      format: "~s"
    },
    bytes: {
      label: "Bytes Transferred",
      func: (d) => d.bytesEnqueued,
      format: "~s"
    },
    none: null
  }
};

var job_info = {
  numLambdas: {
    label: "Workers",
    format: d => d
  },
  numGenerators: {
    label: "Generators",
    format: d => d
  },
  memcachedServers: {
    label: "Memcached",
    format: d => d / 4
  },
  maxDepth: {
    label: "Depth",
    format: d => d
  },
  spp: {
    label: "SPP",
    format: d => d
  },
  outputSize: {
    label: "Size",
    format: d => `${d.x}&times;${d.y}`
  },
  totalUpload: {
    label: "Upload",
    format: d => format_bytes(d, 1)
  },
  totalDownload: {
    label: "Download",
    format: d => format_bytes(d, 1)
  },
  totalSamples: {
    label: "Samples",
    format: d => format_bytes(d, 1)
  },
  
};

var update_jobs_info = (info) => {
  $("#jobsInfoTableBody").html("");

  for (let property in job_info) {
    $("#jobsInfoTableBody").append(`
      <tr>
        <th scope="row">${job_info[property].label}</th>
        <td>${job_info[property].format(info[0][property])}</td>
        <td>${job_info[property].format(info[1][property])}</td>
      </tr>`);
  }
};

var refresh_view = () => {
  Promise.all([
    d3.csv("data/example/a/data.csv"),
    d3.json("data/example/a/info.json"),
    d3.csv("data/example/b/data.csv"),
    d3.json("data/example/b/info.json"),
  ]).then((values) => {
    const [A, B] = [0, 1];
    const colors = ['blue', 'red'];

    var data = [values[0], values[2]];
    var info = [values[1], values[3]];

    update_jobs_info(info);

    var selected = [
      [metrics(info[A])[_state.primary_plot], metrics(info[A])[_state.secondary_plot]],
      [metrics(info[B])[_state.primary_plot], metrics(info[B])[_state.secondary_plot]]];

    var xrange = [0, 0];
    var yrange = [0, 0];
    var y2range = [0, 0];

    for (let i in [A, B]) {
      for (let j in [0, 1]) {
        xrange[0] = Math.min(xrange[0], d3.min(data[i], d => +d.timestampS));
        xrange[1] = Math.max(xrange[1], d3.max(data[i], d => +d.timestampS));
      }

      yrange[0] = Math.min(yrange[0], d3.min(data[i], d => +selected[i][0].func(d)));
      yrange[1] = Math.max(yrange[0], d3.max(data[i], d => +selected[i][0].func(d)));

      if (selected[i][1]) {
        y2range[0] = Math.min(yrange[0], d3.min(data[i], d => +selected[i][1].func(d)));
        y2range[1] = Math.max(yrange[0], d3.max(data[i], d => +selected[i][1].func(d)));
      }
    }

    if (_state.split_view) {
      $("#plot-top").removeClass("h-50 h-100").addClass("h-50");
      $("#plot-bottom").removeClass("h-50 h-0").addClass("h-50");

      var figures = [
        new Figure("#plot-top"),
        new Figure("#plot-bottom")];

      for (var i in [A, B]) {
        figures[i]
          .create_axis("x", xrange, "Time (s)", "")
          .create_axis("y", yrange, selected[i][0].label, selected[i][0].format)
          .path(data[i], d => d.timestampS, selected[i][0].func,
            {
              linecolor: colors[i]
            });

        if (selected[i][1]) {
          figures[i]
            .create_axis("y2", y2range, selected[i][1].label, selected[i][1].format)
            .path(data[i], d => d.timestampS, selected[i][1].func,
              {
                y: 'y2',
                linecolor: 'gray',
                width: 2.5,
                dasharray: ("3, 3")
              });

          if (_state.markers.start) {
            figures[i].annotate_line("x", info[i].initializationTime, "job start");
          }

          if (_state.markers.done_95) {
            figures[i].annotate_line("x",
              completion_time(data[i], 0.95, info[i].totalPaths), "95% done");
          }

          if (_state.markers.done_99) {
            figures[i].annotate_line("x",
              completion_time(data[i], 0.99, info[i].totalPaths), "99% done");
          }
        }
      }
    }
    else {
      $("#plot-top").removeClass("h-50 h-100").addClass("h-100");
      $("#plot-bottom").removeClass("h-50 h-0").addClass("h-0").html("");

      var figure = new Figure("#plot-top");
      figure
        .create_axis("x", xrange, "Time (s)", "")
        .create_axis("y", yrange, selected[0][0].label, selected[0][0].format)
        .path(data[0], d => d.timestampS, selected[0][0].func,
          {
            linecolor: colors[0]
          })
        .path(data[1], d => d.timestampS, selected[1][0].func,
          {
            linecolor: colors[1]
          });

      for (var i in [A, B]) {
        if (_state.markers.start) {
          figure.annotate_line("x", info[i].initializationTime, "job start",
            { color: colors[i], opacity: 0.3 });
        }

        if (_state.markers.done_95) {
          figure.annotate_line("x",
            completion_time(data[i], 0.95, info[i].totalPaths), "95% done",
            { color: colors[i], opacity: 0.3 });
        }

        if (_state.markers.done_99) {
          figure.annotate_line("x",
            completion_time(data[i], 0.99, info[i].totalPaths), "99% done",
            { color: colors[i], opacity: 0.3 });
        }
      }

    }
  });

};

$(document).ready(() => {

  $("#primary-plot-menu a").click((e) => {
    $("#primary-plot-title").html(e.target.innerHTML);
    _state.primary_plot = e.target.getAttribute("plot-id");
    refresh_view();
    e.preventDefault();
  });

  $("#secondary-plot-menu a").click((e) => {
    $("#secondary-plot-title").html(e.target.innerHTML);
    _state.secondary_plot = e.target.getAttribute("plot-id");
    refresh_view();
    e.preventDefault();
  });

  $(".view-state-option").change(function (e) {
    _state.split_view = $("#splitViewCheck").prop('checked');
    _state.markers.start = $("#showJobStartCheck").prop('checked');
    _state.markers.done_95 = $("#show95DoneCheck").prop('checked');
    _state.markers.done_99 = $("#show99DoneCheck").prop('checked');
    refresh_view();
    e.preventDefault();
  });

  refresh_view();
});
