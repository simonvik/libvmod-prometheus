============
vmod-prometheus
============

WIP: Dont use :) And possible slighly unsafe

If you decide to use it anyway then the VCL looks something like:

Example VCL::

	vcl 4.1;
	import prometheus;

	backend be none;

	sub vcl_recv {
		return(synth(900, "Prometheus"));
	}


	sub vcl_synth {
		if(resp.status == 900){
			set resp.http.content-type = "Content-Type: text/plain";
			set resp.status = 200;
			prometheus.render();
			return(deliver);
		}
	}