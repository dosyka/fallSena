(function () {
  "use strict";

  document.addEventListener("DOMContentLoaded", function () {
    var toggle = document.querySelector(".nav__toggle");
    var list = document.getElementById("primary-nav");

    if (!toggle || !list) {
      return;
    }

    toggle.addEventListener("click", function () {
      var isOpen = list.classList.toggle("is-open");
      toggle.setAttribute("aria-expanded", String(isOpen));
    });

    list.addEventListener("click", function (event) {
      if (event.target.closest(".nav__link")) {
        list.classList.remove("is-open");
        toggle.setAttribute("aria-expanded", "false");
      }
    });
  });
})();
