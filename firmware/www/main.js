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
	}
	
	function postCfg( e ) {
			$.post( '/cfg', $(this).serialize(), function(data) {});
			e.preventDefault();
			showmain();
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

		Refresh();
    });
})(jQuery);
