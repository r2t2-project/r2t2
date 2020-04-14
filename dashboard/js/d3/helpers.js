"use strict";

class Figure {
  constructor(box) {
    this.axes = {};

    const margin = { top: 50, left: 70, right: 70, bottom: 50 };
    const width = document.querySelector(box).offsetWidth - margin.left - margin.right - 50;
    const height = document.querySelector(box).offsetHeight - margin.top - margin.bottom - 50;

    d3.select(`${box} > *`).remove();

    this.svg = d3.select(box)
      .append("svg")
      .attr("width", width + margin.left + margin.right)
      .attr("height", height + margin.top + margin.bottom)
      .append("g")
      .attr("transform", `translate(${margin.left},${margin.top})`);

    this.width = width;
    this.height = height;
    this.margin = margin;
  }

  create_axis(type, range, label, format,
    { ticks = null,
      tick_values = null } = {}) {
    var scale = this.axes[type] = d3.scaleLinear().domain(range);

    if (type == "x" || type == "x2") {
      scale.range([0, this.width]);
    }
    else {
      scale.range([this.height, 0]);
    }

    let type_to_axis = {
      x: d3.axisBottom,
      x2: d3.axisTop,
      y: d3.axisLeft,
      y2: d3.axisRight
    };

    let axis = type_to_axis[type](this.axes[type]).tickFormat(d3.format(format));

    if (ticks) axis.ticks(ticks);
    if (tick_values) axis.tickValues(tick_values);

    if (type == "x") {
      this.svg.append("g")
        .attr("transform", `translate(0, ${this.height})`)
        .call(axis);

      // text label for the x axis
      this.svg.append("text")
        .attr("transform", `translate(${this.width / 2}, ${this.height + 40})`)
        .style("text-anchor", "middle")
        .text(label);
    }
    else if (type == "y") {
      this.svg.append("g")
        .call(axis);

      this.svg.append("text")
        .attr("transform", "rotate(-90)")
        .attr("y", 0 - this.margin.left)
        .attr("x", 0 - (this.height / 2))
        .attr("dy", "1em")
        .style("text-anchor", "middle")
        .text(label);
    }
    else if (type == "x2") {

    }
    else if (type == "y2") {
      this.svg.append("g")
        .attr("transform", `translate(${this.width}, 0)`)
        .call(axis);

      this.svg.append("text")
        .attr("transform", "rotate(-90)")
        .attr("y", this.width + this.margin.right)
        .attr("x", 0 - (this.height / 2))
        .attr("dy", "-1em")
        .style("text-anchor", "middle")
        .text(label);
    }

    return this;
  }

  path(data, fx, fy,
    { x = "x", y = "y",
      linecolor = "steelblue",
      width = 1.5,
      dasharray = null } = {}) {
    if (!(x in this.axes)) {
      var range = d3.extent(data, (d) => +fx(d));
      create_axis(x, range, "", "");
    }

    if (!(y in this.axes)) {
      var range = d3.extent(data, (d) => +fy(d));
      create_axis(y, range, "", "");
    }

    this.svg.append("path")
      .datum(data)
      .attr("fill", "none")
      .attr("stroke", linecolor)
      .attr("stroke-width", width)
      .attr("stroke-dasharray", dasharray)
      .attr("d", d3.line()
        .x((d) => this.axes[x](fx(d)))
        .y((d) => this.axes[y](fy(d))));

    return this;
  }

  annotate_line(axis, value, label,
    { color = "#999999", opacity = 1.0 } = {}) {
    const actual = this.axes[axis](value);

    if (axis == "x") {
      this.svg.append("line")
        .attr("stroke", color)
        .attr('stroke-width', 1.0)
        .attr('stroke-opacity', opacity)
        .attr("stroke-dasharray", ("2,4"))
        .attr("x1", actual)
        .attr("x2", actual)
        .attr("y1", 0)
        .attr("y2", this.height);

      this.svg.append("text")
        .attr("transform", "rotate(-90)")
        .attr("y", actual)
        .attr("x", 0)
        .attr("dy", "0.25em")
        .attr("dx", "0.5em")
        .attr("fill", color)
        .attr("fill-opacity", opacity)
        .attr("font-size", "0.8rem")
        .style("text-anchor", "start")
        .text(`${label}`);
    }
    else if (axis == "y") {
      this.svg.append("line")
        .attr("stroke", color)
        .attr('stroke-width', 1.0)
        .attr('stroke-opacity', opacity)
        .attr("stroke-dasharray", ("2,4"))
        .attr("x1", 0)
        .attr("x2", this.width)
        .attr("y1", actual)
        .attr("y2", actual);

      this.svg.append("text")
        .attr("y", actual)
        .attr("x", 0)
        .attr("dy", "-0.4em")
        .attr("dx", "0.5em")
        .attr("fill", color)
        .attr("fill-opacity", opacity)
        .attr("font-size", "0.8rem")
        .style("text-anchor", "start")
        .text(`${label}`);
    }
  }

  marker(from, to, label, { color = "#999999", opacity = 1.0 } = {}) {
    let from_actual = [this.axes.x(from[0]), this.axes.y(from[1])];
    let to_actual = [this.axes.x(to[0]), this.axes.y(to[1])];
    let label_loc = [
      (from_actual[0] + to_actual[0]) / 2,
      (from_actual[1] + to_actual[1]) / 2];

    this.svg.append("line")
      .attr("stroke", color)
      .attr('stroke-width', 1.0)
      .attr('stroke-opacity', opacity)
      .attr("x1", from_actual[0])
      .attr("x2", to_actual[0])
      .attr("y1", from_actual[1])
      .attr("y2", to_actual[1]);

    this.svg.append("line")
      .attr("stroke", color)
      .attr('stroke-width', 1.0)
      .attr('stroke-opacity', opacity)
      .attr("x1", to_actual[0] - 3)
      .attr("x2", to_actual[0] + 3)
      .attr("y1", to_actual[1])
      .attr("y2", to_actual[1]);

    this.svg.append("line")
      .attr("stroke", color)
      .attr('stroke-width', 1.0)
      .attr('stroke-opacity', opacity)
      .attr("x1", from_actual[0] - 3)
      .attr("x2", from_actual[0] + 3)
      .attr("y1", from_actual[1])
      .attr("y2", from_actual[1]);

    this.svg.append("text")
      .attr("y", label_loc[1])
      .attr("x", label_loc[0])
      .attr("dx", "-0.5em")
      .attr("dy", "0.5ex")
      .attr("fill", color)
      .attr("fill-opacity", opacity)
      .attr("font-size", "0.7rem")
      .style("text-anchor", "end")
      .text(`${label}`);
  }

  heatmap(xname, yname, data, fx, { color_start = "white", color_end = "green" } = {}) {
    const width = Math.abs(this.axes.x(1) - this.axes.x(0));
    const height = Math.abs(this.axes.y(1) - this.axes.y(0));

    var colors = d3.scaleLinear()
      .range([color_start, color_end])
      .domain(d3.extent(data, d => +fx(d)));

    this.svg.selectAll()
      .data(data.filter(d => +fx(d) > 0), d => `${d[xname]}:${d[yname]}`)
      .enter()
      .append("rect")
      .attr("x", d => this.axes.x(d[xname]) + width / 2)
      .attr("y", d => this.axes.y(d[yname]) - height)
      .attr("width", width)
      .attr("height", height)
      .style("fill", d => colors(+fx(d)));
  }
};

function format_bytes(bytes, decimals = 2) {
  if (bytes === 0) return '0 B';

  const k = 1024;
  const dm = decimals < 0 ? 0 : decimals;
  const sizes = ['B', 'KiB', 'MiB', 'GiB', 'TiB', 'PiB', 'EiB', 'ZiB', 'YiB'];
  const i = Math.floor(Math.log(bytes) / Math.log(k));

  return parseFloat((bytes / Math.pow(k, i)).toFixed(dm)) + ' ' + sizes[i];
}
