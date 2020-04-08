var create_figure = function (box) {
  const margin = { top: 10, left: 70, right: 70, bottom: 50 };
  const width = document.querySelector(box).offsetWidth - margin.left - margin.right - 70;
  const height = document.querySelector(box).offsetHeight - margin.top - margin.bottom - 70;

  d3.select(`${box} > *`).remove();

  var svg = d3.select(box)
    .append("svg")
    .attr("width", width + margin.left + margin.right)
    .attr("height", height + margin.top + margin.bottom)
    .append("g")
    .attr("transform", `translate(${margin.left},${margin.top})`);

  svg._width = width;
  svg._height = height;
  svg._margin = margin;

  return svg;
};

var plot_graph = function (svg, data, fx, fy,
  { xlabel = "", ylabel = "",
    xrange = null, yrange = null,
    yformat = "", xformat = "",
    linecolor = "steelblue",

    /* secondary */
    fy2 = null, y2label = "", y2range = null, y2format = "" } = {}) {

  if (!xrange) {
    xrange = d3.extent(data, (d) => +fx(d));
  }

  if (!yrange) {
    yrange = d3.extent(data, (d) => +fy(d));
  }

  var x = d3.scaleLinear()
    .domain(xrange)
    .range([0, svg._width]);

  svg._x = x;

  svg.append("g")
    .attr("transform", `translate(0, ${svg._height})`)
    .call(d3.axisBottom(x).tickFormat(d3.format(xformat)));

  // text label for the x axis
  svg.append("text")
    .attr("transform",
      `translate(${svg._width / 2}, ${svg._height + svg._margin.top + 35})`)
    .style("text-anchor", "middle")
    .text(xlabel);

  var y = d3.scaleLinear()
    .domain(yrange)
    .range([svg._height, 0]);

  svg._y = y;

  svg.append("g")
    .call(d3.axisLeft(y).tickFormat(d3.format(yformat)));

  svg.append("text")
    .attr("transform", "rotate(-90)")
    .attr("y", 0 - svg._margin.left)
    .attr("x", 0 - (svg._height / 2))
    .attr("dy", "1em")
    .style("text-anchor", "middle")
    .text(ylabel);

  svg.append("path")
    .datum(data)
    .attr("fill", "none")
    .attr("stroke", linecolor)
    .attr("stroke-width", 1.5)
    .attr("d", d3.line()
      .x((d) => x(fx(d)))
      .y((d) => y(fy(d))));

  if (fy2) {
    if (!y2range) {
      y2range = d3.extent(data, (d) => +fy2(d));
    }

    let y2 = d3.scaleLinear()
      .domain(y2range)
      .range([svg._height, 0]);

    svg.append("g")
      .attr("transform", `translate(${svg._width}, 0)`)
      .call(d3.axisRight(y2)
        .tickFormat(d3.format(y2format)));

    svg.append("text")
      .attr("transform", "rotate(-90)")
      .attr("y", svg._width + svg._margin.right)
      .attr("x", 0 - (svg._height / 2))
      .attr("dy", "-1em")
      .style("text-anchor", "middle")
      .text(y2label);

    svg.append("path")
      .datum(data)
      .attr("fill", "none")
      .attr("stroke", 'gray')
      .attr("stroke-width", 2.5)
      .attr("stroke-dasharray", ("3, 3"))
      .attr("d", d3.line()
        .x((d) => x(fx(d)))
        .y((d) => y2(fy2(d))));
  }
};

var annotate_x = function (svg, x, label) {
  svg.append("line")
    .attr("stroke", '#cccccc')
    .attr('stroke-width', 1.0)
    .attr("stroke-dasharray", ("2,4"))
    .attr("x1", svg._x(x))
    .attr("x2", svg._x(x))
    .attr("y1", 0)
    .attr("y2", svg._height);

  svg.append("text")
    .attr("transform", "rotate(-90)")
    .attr("y", svg._x(x))
    .attr("x", 0)
    .attr("dy", "-0.5em")
    .attr("fill", '#cccccc')
    .attr("font-size", "0.75rem")
    .style("text-anchor", "end")
    .text(`${label} = ${x}`);
};
