/// UI
import Vue from 'vue';
import iView from 'iview';
//import 'lib/iview/dist/styles/iview.css';
//import App from './App.vue';

//Vue.use(iView);

Vue.use(iView, {
    transfer: true,
    size: 'large',
    select: {
        arrow: 'md-arrow-dropdown',
        arrowSize: 20
    }
});

//$(window).load(function () {
//	if(false)//集成版
//	{

//	}
//	else//网页版
//	{
		
//	}
//});

//$(document).ready(function () {
//    console.log("document ready");
	/*始终最大化
    $("#maximize").click(function() {
        console.log("maximize toggled");
        if (typeof window.toggleMaximize == "function") {
            window.toggleMaximize();
        }
    });*/
//	const aaa =  "haha";
//	const tem = `${aaa} lalala`;
//	console.log(tem)

	//const aside =  document.getElementById("control_panel");
	//function setMainLayout()//采用左右布局，动态设置地图大小
	//{  
	//	if(aside.style !== "none")
	//	{
 	//		const page_hight = document.documentElement.clientWidth;//获取页面可见高度
	//		const width = page_hight - aside.clientWidth
    //   		document.getElementById("map_canvas").style.width= width+"px";
	//		console.log("map_canvas");
	//	}
    //}
	//window.onresize=setMainLayout;
	//setMainLayout();
	//const mapContextMenuDiv = document.getElementById('map_context_menu');
	//const markerContextMenuDiv = document.getElementById('marker_context_menu');
//});