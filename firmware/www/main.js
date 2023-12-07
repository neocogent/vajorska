(function($){
	
	var refresh = null, poll = 10000, wantcfg = true;
	
	function Refresh() {
		$.get('/data', { 'cfg': wantcfg }, UpdateData )
            .fail(function() {
                $('#title').css('color', 'red');
            });
       refresh = setTimeout(Refresh, poll);
	}
	
	function UpdateData( data ) {
		if(typeof data != "object")
			$('#title').css('color', 'blue');
		if(data.cfg != undefined) {
			poll = parseInt(data.cfg.poll)*1000;
			wantcfg = false;
		}
	}
	
	function postCfg( e ) {
			$.post( '/cfg', $(this).serialize(), function(data) {

                });
            wantcfg = true;
			e.preventDefault();
			showmain();
	}
	
    function run( e ) {
        $.post( '/run', $(this).serialize(), function(data) {
            
        });
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
