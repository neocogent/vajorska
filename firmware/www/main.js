(function($){
    var active = $('#main');
    var edit_data = null;
    var refresh = idle_state;
    var timer;
    var readout = {
        temp : function (canvas, label, temp) {
            var ctx = canvas.getContext('2d');
            var w = canvas.getBoundingClientRect().width;
            var h = canvas.getBoundingClientRect().height;
            var fh = w/5;
            ctx.font = '${fh}px sans-serif';
            ctx.textAlign = "center";
            ctx.textBaseline = "bottom";
            ctx.clearRect(0,0,w,h);
            ctx.fillText(label+' '+Number(temp).toFixed(1), w/2, (h-fh)/2);
        }
    }    
	function run_state() {
		$.get('/state', {}, function(data) {
            readout.temp($('#pv')[0], "PV", data.tc/2);
            readout.temp($('#sv')[0], "SV", data.tt/2);
            if(!data.active)
                refresh = idle_state;
        },'json');
        timer = setTimeout(refresh, 1000);
	}
    function idle_state() {
        $.get('/state', {}, function(data) {
            readout.temp($('#pv')[0], "PV", data.tc/2);
            readout.temp($('#sv')[0], "SV", data.tt/2); 
        },'json');
        timer = setTimeout(refresh, 1000);
    }
    function run(e) {
        $.post( '/run', $(this).serialize(), function(data) {
            refresh = run_state;
            run_state();
        });
    }
    function on(e) {
        $.post( '/on', $(this).serialize(), function(data) {
            refresh = idle_state;
            idle_state();
        });
    }
    function off(e) {
        $.post( '/off', $(this).serialize(), function(data) {
            refresh = idle_state;
            idle_state();
        });
    }
    function chg(e) {
        var dir = $(this).attr("id");
        $.post( '/chg'+dir, $(this).serialize(), function(data) {
        });
    }
    function fill_dropdown(name) {
        $.get("/"+name, {}, function(data) {
            $('#def-'+name).next().html('');
            $.each(data, function(i,v) { 
                $('#def-'+name).next().append($("<li/>").html('<a href="#">'+v+'</a>'));
                });
            },'json');
    }
    function activate(defbtn, name, post) {
        $(defbtn).html( $(defbtn).text().split(':').shift()+': '+name+' <span class="caret"></span>'); 
        var data = { config: { preset: $('#def-presets').text().split(':').pop().trim() } };
        if(post)
            $.ajax( { url: '/update', type: 'POST', data: JSON.stringify(data), processData: false, contentType: 'application/json; charset=utf-8', 
                    success: function(data) {
                        if(data.status != "OK") {  // indicate error changing config
                        }
                    }
            });
    }
    function getFormDict($form) {
        var obj_array = $form.serializeArray();
        var dict = {};
        $.map(obj_array, function(n, i) {
            if(n['name'] in dict) { 
                if(dict[n['name']] instanceof Array)
                    dict[n['name']].push(+n['value']); // we always want integers in arrays
                else
                    dict[n['name']] = [ +dict[n['name']], +n['value'] ];
            }
            else
                dict[n['name']] = n['value'].trim();
        });
        return dict;
    }
    function update_data(e) {
        var formid = $(this).attr("id");
        formdict = getFormDict($(this).find(':visible'));
        if(document.activeElement.id)
            formdict['op'] = document.activeElement.id;
        var wrapdata = {};
        wrapdata[ formid ] = formdict;
        $.ajax( { url: '/update', type: 'POST', data: JSON.stringify(wrapdata), processData: false, contentType: 'application/json; charset=utf-8', 
            success: function(data) {
                if(data.status == "OK") {
                    fill_dropdown(formid);
                    //activate($('#def-'+formid), formdict['op'] != 'del' ? formdict['name'] : $('#def-'+formid).next().children(':first-child').text(), true);
                } else { // indicate error updating data
                }
            }});
        e.preventDefault();
    }

    $(document).ready( function() {
		$('.menucfg').on('click', function (event) {
			$('#pgs .active').removeClass('active');
			$('#pg-main').tab('show');
			$('#cfgmenu .active').removeClass('active'); 
			$('#cfgmenu ul:first-child').addClass('active'); 
			})

		
        $('.run').click(run);
        $('.on').click(on);
        $('.off').click(off);
        $('.chg').click(chg);
        $('.update').submit(update_data);
        $('.dropdown-menu').on('click', 'li a', function() { activate($(this).parent().parent().prev(), $(this).html(), true); });
        fill_dropdown('presets');
        $.get("/config", {}, function(data) { 
                //activate($('#def-presets'), false);
            },'json');
        timer = setTimeout(refresh, 1000);
    
    });
})(jQuery);
