import { render, screen } from "@testing-library/react";
import { App } from "./App";

test("starts on the connect screen", () => {
  render(<App />);
  expect(screen.getByRole("heading", { name: "Talos" })).toBeTruthy();
  expect(screen.getByTestId("connect")).toBeTruthy();
});
