(function($){
	
	var refresh = null, poll = 10000, datareq = { 'cfg' : true };
	var flowrates = [];
	
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
			$('#ssid').val(data.cfg.ssid);
			$('#pwd').val(data.cfg.pwd);
			$('#mS').val(data.cfg.mS);
			$('#mF').val(data.cfg.mF);
			$('#fR').val(data.cfg.fR);
			$('#sF').val(data.cfg.sF);
			$('#sR').val(data.cfg.sR);
			$('#sD').val(data.cfg.sD);
			$('#hR').val(data.cfg.hR);
			$('#hD').val(data.cfg.hD);
			$('#vR').val(data.cfg.vR);
			flowrates = [];
			flowrates.push(data.cfg.hfr);
			flowrates.push(data.cfg.lfr);
			tanklevels = [];
			tanklevels.push(data.cfg.tf);
			tanklevels.push([]);
			datareq = {};
		}
		// store op data in main screen readouts
		$('#tH .lcdvalue').text(data.temp[0] > 0 ? data.temp[0].toFixed(1)+' °C' : '--');
		$('#tP .lcdvalue').text(data.temp[1] > 0 ? data.temp[1].toFixed(1)+' °C' : '--');
		$('#tM .lcdvalue').text(data.temp[2] > 0 ? data.temp[2].toFixed(1)+' °C' : '--');
		$('#tB .lcdvalue').text(data.temp[3] > 0 ? data.temp[3].toFixed(1)+' °C' : '--');
		$('#tS .lcdvalue').text(data.temp[5] > 0 ? data.temp[5].toFixed(1)+' °C' : '--');
		$('#tT .lcdvalue').text(data.temp[4] > 0 ? data.temp[4].toFixed(1)+' °C' : '--');
		$('#tF .lcdferm1').text(data.temp[6] > 0 ? '1: '+data.temp[6].toFixed(1)+' °C' : '--'); 
		$('#tF .lcdferm2').text(data.temp[7] > 0 ? '2: '+data.temp[7].toFixed(1)+' °C' : '--'); 
		$('#pS .lcdvalue').text(data.steam+' W');
		$('#pH .lcdvalue').text(data.heads+' W');
		$('#fS .lcdvalue').text(data.flows[0]/20+' M');
		$('#fW .lcdvalue').text(data.flows[1]/20+' M'); // missing feed flow for now
		$('#fF .lcdferm1').text('1: '+data.flows[3]/20+' M');
		$('#fF .lcdferm2').text('2: '+data.flows[4]/20+' M');
		
		tanklevels.pop();
		tanklevels.push(data.tn);
		$('#volume').val( tanklevels[ $('#level').find(":selected").val() ][ $('#tank').find(":selected").val() ] );
		
		// add temps cfg dropdown with ids
		var now = $("#tid").prop("selectedIndex");
		$('#tid').empty();
		$.each(data.temp, function(n,e) {
			$('#tid').append($('<option>', { value:n, text:'#'+n+' - '+e+' °C' } ));
			});
		$("#tid").prop("selectedIndex", now);
		$('#volts_now').val(data.volts.toFixed(1));
	}
	function flowchg( e ) {
		$('#flow').val( flowrates[ $('#rate').find(":selected").val() ][ $('#valve').find(":selected").val() ] );
	}
	function tankchg( e ) {
		$('#volume').val( tanklevels[ $('#level').find(":selected").val() ][ $('#tank').find(":selected").val() ] );
	}
	
	function postCfg( e ) {
			$.post( '/cfg', $(this).serialize(), function(data) {});
			e.preventDefault();
			showmain();
	}
	function valveopen( e ) {
		$.post( '/run', { 
			"open": e.target.id[1], 
			"secs": $('#'+e.target.id+'secs').val() 
			}, function(data) {});
		e.preventDefault();
	}
	function voltset( e ) {
		$.post( '/cfg', { "vN":$('#volts_now').val() }, function(data) {});
		e.preventDefault();
	}
	function flowset( e ) {
		$.post( '/cfg', { 
				"valve": $('#valve').find(":selected").val(), 
				"rate": $('#rate').find(":selected").val(), 
				"flow": $('#flow').val() 
				}, function(data) {});
		e.preventDefault();
	}
	function tankset( e ) {
		$.post( '/cfg', { 
				"tank": $('#tank').find(":selected").val(), 
				"level": $('#level').find(":selected").val(), 
				"volume": $('#volume').val() 
				}, function(data) {});
		e.preventDefault();
	}
	function senset( e ) {
		$.post( '/cfg', { 
				"sid": $('#sid').find(":selected").val(), 
				"tid": $('#tid').find(":selected").val() 
				}, function(data) {});
		e.preventDefault();
	}
	
    function run( e ) {
        $.post( '/run', {'on': $(this).is(':checked')}, function(data) {});
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
		$('.btnvalve').on('click', valveopen );
		$('#voltset').on('click', voltset );
		$('#flowset').on('click', flowset );
		$('#tankset').on('click', tankset );
		$('#senset').on('click', senset );
		$('#valve').on('change', flowchg );
		$('#rate').on('change', flowchg );
		$('#tank').on('change', tankchg );
		$('#level').on('change', tankchg );
		$('#timerbtn').on('click', function() { $('#timerpane').removeClass('d-none');});
		$('#timerpane button').on('click', function() { $('#timerpane').addClass('d-none');});
		$('#timer').on('submit', postCfg );
		
		$("a[href='#pg-log']").on('show.bs.tab', function(e) {
			$.get( '/oplog', {}, function(data) {
				$('#oplog').text(data);
				});
		});


		Refresh();
    });
})(jQuery);
