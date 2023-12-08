(function($){
	
	var refresh = null, poll = 10000, datareq = { 'cfg' : true };
	
	function Refresh() {
		$.get('/data', datareq, UpdateData )
            .fail(function() {
                $('#title').css('color', 'red');
            });
       refresh = setTimeout(Refresh, poll);
	}
	
	function UpdateData( data ) {
		if(typeof data != "object")
			$('#title').css('color', 'blue');
		if(data.cfg != undefined) {
			//poll = parseInt(data.cfg.poll)*1000;
			datareq = {};
		}
		// store temps in correct readouts
		$('#tH').text(data.tempC[0]);
		$('#tP').text(data.tempC[1]);
		$('#tM').text(data.tempC[2]);
		$('#tB').text(data.tempC[3]);
		
		// add temps cfg dropdown with ids
		$('#tid').empty();
		$.each(data.cfg.tempC, function(n,e) {
			$('#tid').append($('<option>', { value:n, text:'#'+n+' '+e+' °C' } ));
			});
		$('#volts_now').text(data.volts);
		
	}
	
	function postCfg( e ) {
			$.post( '/cfg', $(this).serialize(), function(data) {});
			e.preventDefault();
			showmain();
	}
	function voltset( e ) {
		$.post( '/cfg', { "vN":$('#volts_now').val() }, function(data) {});
		e.preventDefault();
	}
	function flowset( e ) {
		$.post( '/cfg', { 
				"valve":$('#valve').find(":selected").val(), 
				"rate": $('#flow').find(":selected").val(), 
				"flow": $('#flow').val() 
				}, function(data) {});
		e.preventDefault();
	}
	function senset( e ) {
		$.post( '/cfg', { 
				"sid":$('#sid').find(":selected").val(), 
				"tid":$('#tid').find(":selected").val() 
				}, function(data) {});
		e.preventDefault();
	}
	
    function run( e ) {
        $.post( '/run', {'on':$(this).is(':checked')}, function(data) {});
    }
    
    function showmain(e) {
		$('#pgs .active').removeClass('active');
		$('#pg-main').tab('show');
		$('#cfgmenu .active').removeClass('active'); 
		$('#cfgmenu ul:first-child').addClass('active'); 
		$('#cfgmenu').collapse('hide');
	}
	
    $(document).ready( function() {
		$('#run').on('click', run );
		$('.menucfg').on('click', showmain );
		$('.cancel').on('click', showmain );
		$('.update').on("submit", postCfg);
		$('#voltset').on('click', voltset );
		$('#flowset').on('click', flowset );
		$('#senset').on('click', senset );

		Refresh();
    });
})(jQuery);
