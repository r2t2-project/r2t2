"use strict";

var _state = {
  primary_plot: "rays_enqueued",
  secondary_plot: "none",
  split_view: true,

  markers: {
    start: true,
    done_95: true,
    done_99: true
  },

  scene_list: null,

  scenes: [{
    name: null,
    run: null,
    info: null,
    data: null,
    treelets: null
  },
  {
    name: null,
    run: null,
    info: null,
    data: null,
    treelets: null
  }]
};

const spinner = '<span class="spinner-border spinner-border-sm" role="status" aria-hidden="true"></span>';
const base_url = "https://r2t2-us-west-1.s3-us-west-1.amazonaws.com/dashboard/";

var update_view = () => {
  let scene_buttons = [$("#scene-a"), $("#scene-b")];
  let run_buttons = [$("#run-a"), $("#run-b")];
  let scene_menus = [$("#scene-a + .dropdown-menu"), $("#scene-b + .dropdown-menu")];
  let run_menus = [$("#run-a + .dropdown-menu"), $("#run-b + .dropdown-menu")];

  let set_button_state = (s, button, val) => {
    button.removeClass("disabled d-none");

    switch (s) {
      case "hidden":
        button.addClass("d-none");
        break;

      case "loading":
        button.addClass("disabled");
        button.html(spinner);
        break;

      case "loaded":
        button.html("&mdash;&nbsp;&nbsp; ");
        break;

      case "selected":
        button.html(val);
        break;
    }
  };

  for (let i in [0, 1]) {
    if (_state.scene_list === null) {
      set_button_state("loading", scene_buttons[i]);
      set_button_state("hidden", run_buttons[i]);
    }
    else {
      // scene list is loaded, let's populate the lists first
      scene_menus[i].html("");

      for (let k in _state.scene_list) {
        scene_menus[i].append(`<button class="dropdown-item scene-dropdown-item">${k}</button>`);
      }

      scene_buttons[i].next().children("button").click(function (e) {
        e.preventDefault();

        let clicked = $(this);
        let scene_name = _state.scenes[i].name = clicked.html();

        _state.scenes[i].run
          = _state.scenes[i].info
          = _state.scenes[i].treelet
          = _state.scenes[i].data = null;

        $.ajax(new URL(`${scene_name}/runs.json`, base_url).href, { dataType: "json" })
          .done(data => {
            _state.scene_list[scene_name] = data['runs'];
            update_view();
          })
          .fail(() => { });

        update_view();
      });

      // do we have a scene selected?
      if (_state.scenes[i].name !== null) {
        set_button_state("selected", scene_buttons[i], _state.scenes[i].name);
      } else {
        set_button_state("loaded", scene_buttons[i]);
        /* no scene is selected yet */
        continue;
      }

      /* have we loaded the run list for the scene? */
      if (_state.scene_list[_state.scenes[i].name] !== null) {
        run_menus[i].html("");
        for (let k of _state.scene_list[_state.scenes[i].name]) {
          run_menus[i].append(`<button class="dropdown-item run-dropdown-item"
                                       data-run="${k.name}">${k.name}
          <span class="badge badge-pill badge-light">${k.date}</span></button>`);
        }

        run_buttons[i].next().children("button").click(function (e) {
          e.preventDefault();

          let clicked = $(this);
          let run_name = _state.scenes[i].run = clicked.attr('data-run');
          _state.scenes[i].info = _state.scenes[i].treelet = _state.scenes[i].data = null;
          update_view();
        });
      } else {
        set_button_state("loading", run_buttons[i]);
        continue;
      }

      if (_state.scenes[i].run !== null) {
        set_button_state("selected", run_buttons[i], _state.scenes[i].run);
      } else {
        set_button_state("loaded", run_buttons[i]);
        continue;
      }
    }
  }

  // do we need to plot anything?
  var things_to_load = [];

  for (let k of _state.scenes) {
    if (k.name === null || k.run === null) {
      things_to_load.push(null, null, null);
      continue;
    }

    if (k.info === null) {
      things_to_load.push(d3.json(new URL(`${k.name}/${k.run}/info.json`, base_url).href));
    } else {
      things_to_load.push(null);
    }

    if (k.data === null) {
      things_to_load.push(d3.csv(new URL(`${k.name}/${k.run}/data.csv`, base_url).href));
    } else {
      things_to_load.push(null);
    }

    if (k.treelet === null && _state.primary_plot.startsWith("treelet_")) {
      things_to_load.push(d3.csv(new URL(`${k.name}/${k.run}/treelet.csv`, base_url).href));
    } else {
      things_to_load.push(null);
    }
  }

  Promise.all(things_to_load)
    .then(values => {
      if (values[0]) _state.scenes[0].info = values[0];
      if (values[1]) _state.scenes[0].data = values[1];
      if (values[2]) _state.scenes[0].treelet = values[2];
      if (values[3]) _state.scenes[1].info = values[3];
      if (values[4]) _state.scenes[1].data = values[4];
      if (values[5]) _state.scenes[1].treelet = values[5];

      update_graphs();
    });
};

var completion_time = (data, f, total_paths) => {
  return d3.bisector(d => +d.runningCompletion)
    .right(data, f * total_paths);
};

var metrics = (info) => {
  return {
    paths_finished: {
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
    bag_sizes: {
      label: "Average Bag Size (bytes)",
      func: (d) => d.bytesPerBag_mean,
      format: "~s"
    },
    bytes_sent: {
      label: "Bytes Sent",
      func: (d) => d.bytesEnqueued,
      format: "~s"
    },
    bytes_received: {
      label: "Bytes Received",
      func: (d) => d.bytesDequeued,
      format: "~s"
    },
    bytes_transferred: {
      label: "Bytes Transferred",
      func: (d) => d.totalTransferred,
      format: "~s"
    },
    cpu_mean: {
      label: "Average CPU Usage (%)",
      func: (d) => d.cpuUsage_mean,
      format: "~s"
    },
    cpu_median: {
      label: "Median CPU Usage (%)",
      func: (d) => d.cpuUsage_median,
      format: "~s"
    },
    cpu_p90: {
      label: "CPU Usage (90-percentile, %)",
      func: (d) => d.cpuUsage_p90,
      format: "~s"
    },
    cpu_p95: {
      label: "CPU Usage (90-percentile, %)",
      func: (d) => d.cpuUsage_p90,
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
  treeletCount: {
    label: "Treelets",
    format: d => d
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
    label: "Dimensions",
    format: d => `${d.x}&times;${d.y}`
  },
  baggingDelay: {
    label: "Bagging Delay",
    format: d => `${d ? d : "&mdash;"} ms`
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
  generationTime: {
    label: "⏱️ Generation",
    format: d => `${d.toFixed(2)} s`
  },
  initializationTime: {
    label: "⏱️ Initialization",
    format: d => `${d.toFixed(2)} s`
  },
  tracingTime: {
    label: "⏱️ Ray Tracing",
    format: d => `${d.toFixed(2)} s`
  },
  estimatedCost: {
    label: "Cost",
    format: d => `${d ? "$" + d : "&mdash;"}`
  }
};

var update_jobs_info = (info) => {
  $("#jobs-info table tbody").html("");

  if (!info || (!info[0] && !info[1])) {
    $("#jobs-info").addClass("d-none");
  } else {
    $("#jobs-info").removeClass("d-none");
  }

  for (let property in job_info) {
    $("#jobs-info table tbody").append(`
      <tr>
        <th scope="row">${job_info[property].label}</th>
        <td>${info[0] ? job_info[property].format(info[0][property]) : ""}</td>
        <td>${info[1] ? job_info[property].format(info[1][property]) : ""}</td>
      </tr>`);
  }
};

const range = (start, stop, step = 1) =>
  Array(Math.ceil((stop - start) / step))
    .fill(start)
    .map((x, y) => x + y * step);

var update_graphs = () => {
  const colors = ['blue', 'red'];

  var data = [_state.scenes[0].data, _state.scenes[1].data];
  var info = [_state.scenes[0].info, _state.scenes[1].info];
  var treelet = [_state.scenes[0].treelet, _state.scenes[1].treelet];

  update_jobs_info(info);

  if (!(data[0] || data[1])) {
    return;
  }

  if (_state.primary_plot.startsWith("treelet_")) {
    $("#plot-top").removeClass("h-50 h-100").addClass("h-50").html("");
    $("#plot-bottom").removeClass("h-50 h-0").addClass("h-50").html("");

    var figures = [
      new Figure("#plot-top"),
      new Figure("#plot-bottom")];

    let max_treelet_id = 0;
    let max_timestamp = 0;

    for (var i in [0, 1]) {
      if (!treelet[i]) {
        continue;
      }

      max_treelet_id = Math.max(max_treelet_id, d3.max(treelet[i], d => +d.treeletId));
      max_timestamp = Math.max(max_timestamp, d3.max(treelet[i], d => +d.timestampS));
    }

    for (var i in [0, 1]) {
      if (!treelet[i]) {
        continue;
      }

      figures[i].create_axis("x", [0, max_timestamp], "Time (s)", "")
        .create_axis("y", [0, max_treelet_id], "Treelet ID", "")
        .heatmap("timestampS", "treeletId", treelet[i], d => d.raysDequeued,
          { color_start: "#eeeeee", color_end: colors[i] });

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

    return;
  }

  var xrange = [Infinity, 0];
  var yrange = [Infinity, 0];
  var y2range = [Infinity, 0];

  for (var i in [0, 1]) {
    if (!data[i]) {
      continue;
    }

    let primary_metric = metrics(info[i])[_state.primary_plot];
    let secondary_metric = metrics(info[i])[_state.secondary_plot];

    xrange[0] = Math.min(xrange[0], d3.min(data[i], d => +d.timestampS));
    xrange[1] = Math.max(xrange[1], d3.max(data[i], d => +d.timestampS));

    yrange[0] = Math.min(yrange[0], d3.min(data[i], d => +primary_metric.func(d)));
    yrange[1] = Math.max(yrange[1], d3.max(data[i], d => +primary_metric.func(d)));

    if (secondary_metric) {
      y2range[0] = Math.min(y2range[0], d3.min(data[i], d => +secondary_metric.func(d)));
      y2range[1] = Math.max(y2range[1], d3.max(data[i], d => +secondary_metric.func(d)));
    }
  }

  if (_state.split_view) {
    $("#plot-top").removeClass("h-50 h-100").addClass("h-50").html("");
    $("#plot-bottom").removeClass("h-50 h-0").addClass("h-50").html("");

    var figures = [
      new Figure("#plot-top"),
      new Figure("#plot-bottom")];

    for (var i in [0, 1]) {
      if (!data[i]) {
        continue;
      }

      let primary_metric = metrics(info[i])[_state.primary_plot];
      let secondary_metric = metrics(info[i])[_state.secondary_plot];

      figures[i]
        .create_axis("x", xrange, "Time (s)", "")
        .create_axis("y", yrange, primary_metric.label, primary_metric.format)
        .path(data[i], d => d.timestampS, primary_metric.func,
          {
            linecolor: colors[i]
          });

      if (secondary_metric) {
        figures[i]
          .create_axis("y2", y2range, secondary_metric.label, secondary_metric.format)
          .path(data[i], d => d.timestampS, secondary_metric.func,
            {
              y: 'y2',
              linecolor: 'gray',
              width: 2.5,
              dasharray: ("3, 3")
            });
      }

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
  else {
    $("#plot-top").removeClass("h-50 h-100").addClass("h-100").html("");
    $("#plot-bottom").removeClass("h-50 h-0").addClass("h-0").html("");

    let label = info[0] ? metrics(info[0])[_state.primary_plot].label :
      metrics(info[1])[_state.primary_plot].label;

    let format = info[0] ? metrics(info[0])[_state.primary_plot].format :
      metrics(info[1])[_state.primary_plot].format;

    var figure = new Figure("#plot-top");
    figure
      .create_axis("x", xrange, "Time (s)", "")
      .create_axis("y", yrange, label, format);

    if (data[0]) {
      figure.path(data[0], d => d.timestampS, metrics(info[0])[_state.primary_plot].func,
        {
          linecolor: colors[0]
        });
    }

    if (data[1]) {
      figure.path(data[1], d => d.timestampS, metrics(info[1])[_state.primary_plot].func,
        {
          linecolor: colors[1]
        });
    }

    for (var i in [0, 1]) {
      if (!info[i]) {
        continue;
      }

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
};

$(document).ready(() => {

  // get the list of scenes

  $.ajax(new URL('scenes.json', base_url).href, { dataType: "json" })
    .done(data => {
      _state.scene_list = {};

      for (let i in data['scenes']) {
        _state.scene_list[data['scenes'][i]] = null;
      }

      update_view();
    })
    .fail(() => { });


  update_view();

  $("#primary-plot-menu a").click((e) => {
    $("#primary-plot-title").html(e.target.innerHTML);
    _state.primary_plot = e.target.getAttribute("plot-id");
    update_view();
    e.preventDefault();
  });

  $("#secondary-plot-menu a").click((e) => {
    $("#secondary-plot-title").html(e.target.innerHTML);
    _state.secondary_plot = e.target.getAttribute("plot-id");
    update_view();
    e.preventDefault();
  });

  $(".view-state-option").change(function (e) {
    _state.split_view = $("#splitViewCheck").prop('checked');
    _state.markers.start = $("#showJobStartCheck").prop('checked');
    _state.markers.done_95 = $("#show95DoneCheck").prop('checked');
    _state.markers.done_99 = $("#show99DoneCheck").prop('checked');
    update_view();
    e.preventDefault();
  });
});
