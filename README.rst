============
vmod-prometheus
============

WIP: Dont use :) And possible slighly unsafe

If you decide to use it anyway then the VCL looks something like:

Add "-a :9101" to your varnish command line

Example VCL::

	vcl 4.1;
	import std;
	import prometheus;

	backend be none;

	sub vcl_recv {
		if(req.url == "/metric" && std.port(server.ip) == 9101) {
			return(synth(900, "Prometheus"));
		}
	}


	sub vcl_synth {
		if(resp.status == 900){
			set resp.http.content-type = "Content-Type: text/plain; version=0.0.4; charset=utf-8";
			set resp.status = 200;
			prometheus.render();
			return(deliver);
		}
	}