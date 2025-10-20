import matplotlib.pyplot as plt
import pandas


df1 = pandas.read_csv('./Data/Device1.csv', names=['x','y','z'])
df2 = pandas.read_csv('./Data/Device2.csv', names=['x','y','z'])

print(df1.head())

if len(df1['x']) == 0:
    print("test.csv is empty.")
    exit()

fig, axes = plt.subplots(3, 2, sharex=True, figsize=(10, 7))
axes[0][0].plot(df1['x'], label="X1")
axes[0][0].set_ylabel("X")
axes[0][0].legend(loc="upper right")

axes[1][0].plot(df1['y'], label="Y1", color="tab:orange")
axes[1][0].set_ylabel("Y")
axes[1][0].legend(loc="upper right")

axes[2][0].plot(df1['z'], label="Z1", color="tab:green")
axes[2][0].set_ylabel("Z")
axes[2][0].set_xlabel("Sample")
axes[2][0].legend(loc="upper right")

axes[0][1].plot(df2['x'], label="X2")
axes[0][1].set_ylabel("X")
axes[0][1].legend(loc="upper right")

axes[1][1].plot(df2['y'], label="Y2", color="tab:orange")
axes[1][1].set_ylabel("Y")
axes[1][1].legend(loc="upper right")

axes[2][1].plot(df2['z'], label="Z2", color="tab:green")
axes[2][1].set_ylabel("Z")
axes[2][1].set_xlabel("Sample")
axes[2][1].legend(loc="upper right")


for n in range(len(df1['x'])//100):
    axes[0][0].axvline(n*100)

plt.tight_layout()
plt.show()