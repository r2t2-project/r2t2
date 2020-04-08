"use strict";

class Figure {
  constructor(box) {
    this.axes = {};

    const margin = { top: 10, left: 70, right: 70, bottom: 50 };
    const width = document.querySelector(box).offsetWidth - margin.left - margin.right - 70;
    const height = document.querySelector(box).offsetHeight - margin.top - margin.bottom - 70;

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

  create_axis(type, range, label, format) {
    var axis = this.axes[type] = d3.scaleLinear().domain(range);

    if (type == "x" || type == "x2") {
      axis.range([0, this.width]);
    }
    else {
      axis.range([this.height, 0]);
    }

    if (type == "x") {
      this.svg.append("g")
        .attr("transform", `translate(0, ${this.height})`)
        .call(d3.axisBottom(this.axes.x).tickFormat(d3.format(format)));

      // text label for the x axis
      this.svg.append("text")
        .attr("transform", `translate(${this.width / 2}, ${this.height + this.margin.top + 35})`)
        .style("text-anchor", "middle")
        .text(label);
    }
    else if (type == "y") {
      this.svg.append("g")
        .call(d3.axisLeft(this.axes.y).tickFormat(d3.format(format)));

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
        .call(d3.axisRight(this.axes.y2)
          .tickFormat(d3.format(format)));

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

  annotate_line(axis, value, label) {
    const actual = this.axes[axis](value);

    if (axis == "x") {
      this.svg.append("line")
        .attr("stroke", '#cccccc')
        .attr('stroke-width', 1.0)
        .attr("stroke-dasharray", ("2,4"))
        .attr("x1", actual)
        .attr("x2", actual)
        .attr("y1", 0)
        .attr("y2", this.height);

      this.svg.append("text")
        .attr("transform", "rotate(-90)")
        .attr("y", actual)
        .attr("x", 0 - this.height / 2)
        .attr("dy", "-0.5em")
        .attr("fill", '#cccccc')
        .attr("font-size", "0.75rem")
        .style("text-anchor", "middle")
        .text(`${label}`);
    }
  }
};
